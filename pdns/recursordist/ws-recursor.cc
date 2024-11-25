/*
 * This file is part of PowerDNS or dnsdist.
 * Copyright -- PowerDNS.COM B.V. and its contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * In addition, for the avoidance of any doubt, permission is granted to
 * link this program with OpenSSL and to (re)distribute the binaries
 * produced as the result of such linking.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "ws-recursor.hh"
#include "json.hh"

#include <algorithm>
#include <string>
#include "namespaces.hh"
#include <iostream>
#include "iputils.hh"
#include "rec_channel.hh"
#include "rec_metrics.hh"
#include "arguments.hh"
#include "misc.hh"
#include "syncres.hh"
#include "dnsparser.hh"
#include "json11.hpp"
#include "webserver.hh"
#include "ws-api.hh"
#include "logging.hh"
#include "rec-lua-conf.hh"
#include "rpzloader.hh"
#include "uuid-utils.hh"
#include "tcpiohandler.hh"
#include "rec-main.hh"
#include "settings/cxxsettings.hh" // IWYU pragma: keep, needed by included generated file
#include "settings/rust/web.rs.h"

using json11::Json;

void productServerStatisticsFetch(map<string, string>& out)
{
  auto stats = getAllStatsMap(StatComponent::API);
  map<string, string> ret;
  for (const auto& entry : stats) {
    ret.emplace(entry.first, entry.second.d_value);
  }
  out.swap(ret);
}

std::optional<uint64_t> productServerStatisticsFetch(const std::string& name)
{
  return getStatByName(name);
}

static void apiWriteConfigFile(const string& filebasename, const string& content)
{
  if (::arg()["api-config-dir"].empty()) {
    throw ApiException("Config Option \"api-config-dir\" must be set");
  }

  string filename = ::arg()["api-config-dir"] + "/" + filebasename;
  if (g_yamlSettings) {
    filename += ".yml";
  }
  else {
    filename += ".conf";
  }
  ofstream ofconf(filename);
  if (!ofconf) {
    throw ApiException("Could not open config fragment file '" + filename + "' for writing: " + stringerror());
  }
  ofconf << "# Generated by pdns-recursor REST API, DO NOT EDIT" << endl;
  ofconf << content << endl;
  ofconf.close();
}

static void apiServerConfigACLGET(const std::string& aclType, HttpRequest* /* req */, HttpResponse* resp)
{
  // Return currently configured ACLs
  vector<string> entries;
  if (t_allowFrom && aclType == "allow-from") {
    entries = t_allowFrom->toStringVector();
  }
  else if (t_allowNotifyFrom && aclType == "allow-notify-from") {
    entries = t_allowNotifyFrom->toStringVector();
  }

  resp->setJsonBody(Json::object{
    {"name", aclType},
    {"value", entries},
  });
}

static void apiServerConfigACLPUT(const std::string& aclType, HttpRequest* req, HttpResponse* resp)
{
  const auto& document = req->json();

  const auto& jlist = document["value"];

  if (!jlist.is_array()) {
    throw ApiException("'value' must be an array");
  }

  if (g_yamlSettings) {
    ::rust::Vec<::rust::String> vec;
    for (const auto& value : jlist.array_items()) {
      vec.emplace_back(value.string_value());
    }

    try {
      ::pdns::rust::settings::rec::validate_allow_from(aclType, vec);
    }
    catch (const ::rust::Error& e) {
      throw ApiException(string("Unable to convert: ") + e.what());
    }
    ::rust::String yaml;
    if (aclType == "allow-from") {
      yaml = pdns::rust::settings::rec::allow_from_to_yaml_string_incoming("allow_from", "allow_from_file", vec);
    }
    else {
      yaml = pdns::rust::settings::rec::allow_from_to_yaml_string_incoming("allow_notify_from", "allow_notify_from_file", vec);
    }
    apiWriteConfigFile(aclType, string(yaml));
  }
  else {
    NetmaskGroup nmg;
    for (const auto& value : jlist.array_items()) {
      try {
        nmg.addMask(value.string_value());
      }
      catch (const NetmaskException& e) {
        throw ApiException(e.reason);
      }
    }

    ostringstream strStream;

    // Clear <foo>-from-file if set, so our changes take effect
    strStream << aclType << "-file=" << endl;

    // Clear ACL setting, and provide a "parent" value
    strStream << aclType << "=" << endl;
    strStream << aclType << "+=" << nmg.toString() << endl;

    apiWriteConfigFile(aclType, strStream.str());
  }

  parseACLs();

  apiServerConfigACLGET(aclType, req, resp);
}

static void apiServerConfigAllowFromGET(HttpRequest* req, HttpResponse* resp)
{
  apiServerConfigACLGET("allow-from", req, resp);
}

static void apiServerConfigAllowNotifyFromGET(HttpRequest* req, HttpResponse* resp)
{
  apiServerConfigACLGET("allow-notify-from", req, resp);
}

static void apiServerConfigAllowFromPUT(HttpRequest* req, HttpResponse* resp)
{
  apiServerConfigACLPUT("allow-from", req, resp);
}

static void apiServerConfigAllowNotifyFromPUT(HttpRequest* req, HttpResponse* resp)
{
  apiServerConfigACLPUT("allow-notify-from", req, resp);
}

static void fillZone(const DNSName& zonename, HttpResponse* resp)
{
  auto iter = SyncRes::t_sstorage.domainmap->find(zonename);
  if (iter == SyncRes::t_sstorage.domainmap->end()) {
    throw ApiException("Could not find domain '" + zonename.toLogString() + "'");
  }

  const SyncRes::AuthDomain& zone = iter->second;

  Json::array servers;
  for (const ComboAddress& server : zone.d_servers) {
    servers.emplace_back(server.toStringWithPort());
  }

  Json::array records;
  for (const SyncRes::AuthDomain::records_t::value_type& record : zone.d_records) {
    records.push_back(Json::object{
      {"name", record.d_name.toString()},
      {"type", DNSRecordContent::NumberToType(record.d_type)},
      {"ttl", (double)record.d_ttl},
      {"content", record.getContent()->getZoneRepresentation()}});
  }

  // id is the canonical lookup key, which doesn't actually match the name (in some cases)
  string zoneId = apiZoneNameToId(iter->first);
  Json::object doc = {
    {"id", zoneId},
    {"url", "/api/v1/servers/localhost/zones/" + zoneId},
    {"name", iter->first.toString()},
    {"kind", zone.d_servers.empty() ? "Native" : "Forwarded"},
    {"servers", servers},
    {"recursion_desired", zone.d_servers.empty() ? false : zone.d_rdForward},
    {"notify_allowed", isAllowNotifyForZone(zonename)},
    {"records", records}};

  resp->setJsonBody(doc);
}

static void doCreateZone(const Json& document)
{
  if (::arg()["api-config-dir"].empty()) {
    throw ApiException("Config Option \"api-config-dir\" must be set");
  }

  const DNSName zone = apiNameToDNSName(stringFromJson(document, "name"));
  const string zonename = zone.toString();
  apiCheckNameAllowedCharacters(zonename);

  string singleIPTarget = document["single_target_ip"].string_value();
  string kind = toUpper(stringFromJson(document, "kind"));
  bool rdFlag = boolFromJson(document, "recursion_desired");
  bool notifyAllowed = boolFromJson(document, "notify_allowed", false);
  string confbasename = "zone-" + apiZoneNameToId(zone);

  const string yamlAPIZonesFile = ::arg()["api-config-dir"] + "/apizones";

  if (kind == "NATIVE") {
    if (rdFlag) {
      throw ApiException("kind=Native and recursion_desired are mutually exclusive");
    }
    if (!singleIPTarget.empty()) {
      try {
        ComboAddress rem(singleIPTarget);
        if (rem.sin4.sin_family != AF_INET) {
          throw ApiException("");
        }
        singleIPTarget = rem.toString();
      }
      catch (...) {
        throw ApiException("Single IP target '" + singleIPTarget + "' is invalid");
      }
    }
    string zonefilename = ::arg()["api-config-dir"] + "/" + confbasename + ".zone";
    ofstream ofzone(zonefilename.c_str());
    if (!ofzone) {
      throw ApiException("Could not open '" + zonefilename + "' for writing: " + stringerror());
    }
    ofzone << "; Generated by pdns-recursor REST API, DO NOT EDIT" << endl;
    ofzone << zonename << "\tIN\tSOA\tlocal.zone.\thostmaster." << zonename << " 1 1 1 1 1" << endl;
    if (!singleIPTarget.empty()) {
      ofzone << zonename << "\t3600\tIN\tA\t" << singleIPTarget << endl;
      ofzone << "*." << zonename << "\t3600\tIN\tA\t" << singleIPTarget << endl;
    }
    ofzone.close();

    if (g_yamlSettings) {
      pdns::rust::settings::rec::AuthZone authzone;
      authzone.zone = zonename;
      authzone.file = zonefilename;
      pdns::rust::settings::rec::api_add_auth_zone(yamlAPIZonesFile, std::move(authzone));
    }
    else {
      apiWriteConfigFile(confbasename, "auth-zones+=" + zonename + "=" + zonefilename);
    }
  }
  else if (kind == "FORWARDED") {
    if (g_yamlSettings) {
      pdns::rust::settings::rec::ForwardZone forward;
      forward.zone = zonename;
      forward.recurse = rdFlag;
      forward.notify_allowed = notifyAllowed;
      for (const auto& value : document["servers"].array_items()) {
        forward.forwarders.emplace_back(value.string_value());
      }
      pdns::rust::settings::rec::api_add_forward_zone(yamlAPIZonesFile, std::move(forward));
    }
    else {
      string serverlist;
      for (const auto& value : document["servers"].array_items()) {
        const string& server = value.string_value();
        if (server.empty()) {
          throw ApiException("Forwarded-to server must not be an empty string");
        }
        try {
          ComboAddress address = parseIPAndPort(server, 53);
          if (!serverlist.empty()) {
            serverlist += ";";
          }
          serverlist += address.toStringWithPort();
        }
        catch (const PDNSException& e) {
          throw ApiException(e.reason);
        }
      }
      if (serverlist.empty()) {
        throw ApiException("Need at least one upstream server when forwarding");
      }

      const string notifyAllowedConfig = notifyAllowed ? "\nallow-notify-for+=" + zonename : "";
      if (rdFlag) {
        apiWriteConfigFile(confbasename, "forward-zones-recurse+=" + zonename + "=" + serverlist + notifyAllowedConfig);
      }
      else {
        apiWriteConfigFile(confbasename, "forward-zones+=" + zonename + "=" + serverlist + notifyAllowedConfig);
      }
    }
  }
  else {
    throw ApiException("invalid kind");
  }
}

static bool doDeleteZone(const DNSName& zonename)
{
  if (::arg()["api-config-dir"].empty()) {
    throw ApiException("Config Option \"api-config-dir\" must be set");
  }

  string filename;
  if (g_yamlSettings) {
    const string yamlAPiZonesFile = ::arg()["api-config-dir"] + "/apizones";
    pdns::rust::settings::rec::api_delete_zone(yamlAPiZonesFile, zonename.toString());
  }
  else {
    // this one must exist
    filename = ::arg()["api-config-dir"] + "/zone-" + apiZoneNameToId(zonename) + ".conf";
    if (unlink(filename.c_str()) != 0) {
      return false;
    }
  }
  // .zone file is optional
  filename = ::arg()["api-config-dir"] + "/zone-" + apiZoneNameToId(zonename) + ".zone";
  unlink(filename.c_str());

  return true;
}

static void apiServerZonesPOST(HttpRequest* req, HttpResponse* resp)
{
  if (::arg()["api-config-dir"].empty()) {
    throw ApiException("Config Option \"api-config-dir\" must be set");
  }

  Json document = req->json();

  DNSName zonename = apiNameToDNSName(stringFromJson(document, "name"));
  {
    auto map = g_initialDomainMap.lock();
    const auto& iter = (*map)->find(zonename);
    if (iter != (*map)->cend()) {
      throw ApiException("Zone already exists");
    }
    doCreateZone(document);
  }
  reloadZoneConfiguration(g_yamlSettings);
  fillZone(zonename, resp);
  resp->status = 201;
}

static void apiServerZonesGET(HttpRequest* /* req */, HttpResponse* resp)
{
  Json::array doc;
  auto lock = g_initialDomainMap.lock();
  for (const auto& val : **lock) {
    const SyncRes::AuthDomain& zone = val.second;
    Json::array servers;
    for (const auto& server : zone.d_servers) {
      servers.emplace_back(server.toStringWithPort());
    }
    // id is the canonical lookup key, which doesn't actually match the name (in some cases)
    string zoneId = apiZoneNameToId(val.first);
    doc.push_back(Json::object{
      {"id", zoneId},
      {"url", "/api/v1/servers/localhost/zones/" + zoneId},
      {"name", val.first.toString()},
      {"kind", zone.d_servers.empty() ? "Native" : "Forwarded"},
      {"servers", servers},
      {"recursion_desired", zone.d_servers.empty() ? false : zone.d_rdForward}});
  }
  resp->setJsonBody(doc);
}

static inline DNSName findZoneById(HttpRequest* req)
{
  auto zonename = apiZoneIdToName(req->parameters["id"]);
  if (SyncRes::t_sstorage.domainmap->find(zonename) == SyncRes::t_sstorage.domainmap->end()) {
    throw ApiException("Could not find domain '" + zonename.toLogString() + "'");
  }
  return zonename;
}

static void apiServerZoneDetailPUT(HttpRequest* req, HttpResponse* resp)
{
  auto zonename = findZoneById(req);
  const auto& document = req->json();

  doDeleteZone(zonename);
  doCreateZone(document);
  reloadZoneConfiguration(g_yamlSettings);
  resp->body = "";
  resp->status = 204; // No Content, but indicate success
}

static void apiServerZoneDetailDELETE(HttpRequest* req, HttpResponse* resp)
{
  auto zonename = findZoneById(req);
  if (!doDeleteZone(zonename)) {
    throw ApiException("Deleting domain failed");
  }

  reloadZoneConfiguration(g_yamlSettings);
  // empty body on success
  resp->body = "";
  resp->status = 204; // No Content: declare that the zone is gone now
}

static void apiServerZoneDetailGET(HttpRequest* req, HttpResponse* resp)
{
  auto zonename = findZoneById(req);
  fillZone(zonename, resp);
}

static void apiServerSearchData(HttpRequest* req, HttpResponse* resp)
{
  string qVar = req->getvars["q"];
  if (qVar.empty()) {
    throw ApiException("Query q can't be blank");
  }

  Json::array doc;
  for (const SyncRes::domainmap_t::value_type& val : *SyncRes::t_sstorage.domainmap) {
    string zoneId = apiZoneNameToId(val.first);
    string zoneName = val.first.toString();
    if (pdns_ci_find(zoneName, qVar) != string::npos) {
      doc.push_back(Json::object{
        {"type", "zone"},
        {"zone_id", zoneId},
        {"name", zoneName}});
    }

    // if zone name is an exact match, don't bother with returning all records/comments in it
    if (val.first == DNSName(qVar)) {
      continue;
    }

    const SyncRes::AuthDomain& zone = val.second;

    for (const SyncRes::AuthDomain::records_t::value_type& resourceRec : zone.d_records) {
      if (pdns_ci_find(resourceRec.d_name.toString(), qVar) == string::npos && pdns_ci_find(resourceRec.getContent()->getZoneRepresentation(), qVar) == string::npos) {
        continue;
      }

      doc.push_back(Json::object{
        {"type", "record"},
        {"zone_id", zoneId},
        {"zone_name", zoneName},
        {"name", resourceRec.d_name.toString()},
        {"content", resourceRec.getContent()->getZoneRepresentation()}});
    }
  }
  resp->setJsonBody(doc);
}

static void apiServerCacheFlush(HttpRequest* req, HttpResponse* resp)
{
  DNSName canon = apiNameToDNSName(req->getvars["domain"]);
  bool subtree = req->getvars.count("subtree") > 0 && req->getvars["subtree"] == "true";
  uint16_t qtype = 0xffff;
  if (req->getvars.count("type") != 0) {
    qtype = QType::chartocode(req->getvars["type"].c_str());
  }

  struct WipeCacheResult res = wipeCaches(canon, subtree, qtype);
  resp->setJsonBody(Json::object{
    {"count", res.record_count + res.packet_count + res.negative_record_count},
    {"result", "Flushed cache."}});
}

static void apiServerRPZStats(HttpRequest* /* req */, HttpResponse* resp)
{
  auto luaconf = g_luaconfs.getLocal();
  auto numZones = luaconf->dfe.size();

  Json::object ret;

  for (size_t i = 0; i < numZones; i++) {
    auto zone = luaconf->dfe.getZone(i);
    if (zone == nullptr) {
      continue;
    }
    const auto& name = zone->getName();
    auto stats = getRPZZoneStats(name);
    if (stats == nullptr) {
      continue;
    }
    Json::object zoneInfo = {
      {"transfers_failed", (double)stats->d_failedTransfers},
      {"transfers_success", (double)stats->d_successfulTransfers},
      {"transfers_full", (double)stats->d_fullTransfers},
      {"records", (double)stats->d_numberOfRecords},
      {"last_update", (double)stats->d_lastUpdate},
      {"serial", (double)stats->d_serial},
    };
    ret[name] = zoneInfo;
  }
  resp->setJsonBody(ret);
}

static void prometheusMetrics(HttpRequest* /* req */, HttpResponse* resp)
{
  static MetricDefinitionStorage s_metricDefinitions;

  std::ostringstream output;

  // Argument controls disabling of any stats. So
  // stats-api-disabled-list will be used to block returned stats.
  auto varmap = getAllStatsMap(StatComponent::API);
  for (const auto& tup : varmap) {
    std::string metricName = tup.first;
    std::string prometheusMetricName = tup.second.d_prometheusName;
    std::string helpname = tup.second.d_prometheusName;
    MetricDefinition metricDetails;

    if (s_metricDefinitions.getMetricDetails(metricName, metricDetails)) {
      std::string prometheusTypeName = MetricDefinitionStorage::getPrometheusStringMetricType(
        metricDetails.d_prometheusType);

      if (prometheusTypeName.empty()) {
        continue;
      }
      if (metricDetails.d_prometheusType == PrometheusMetricType::multicounter) {
        helpname = prometheusMetricName.substr(0, prometheusMetricName.find('{'));
      }
      else if (metricDetails.d_prometheusType == PrometheusMetricType::histogram) {
        helpname = prometheusMetricName.substr(0, prometheusMetricName.find('{'));
        // name is XXX_count, strip the _count part
        helpname = helpname.substr(0, helpname.length() - 6);
      }
      output << "# TYPE " << helpname << " " << prometheusTypeName << "\n";
      output << "# HELP " << helpname << " " << metricDetails.d_description << "\n";
    }
    output << prometheusMetricName << " " << tup.second.d_value << "\n";
  }

  output << "# HELP pdns_recursor_info "
         << "Info from pdns_recursor, value is always 1"
         << "\n";
  output << "# TYPE pdns_recursor_info "
         << "gauge"
         << "\n";
  output << "pdns_recursor_info{version=\"" << VERSION << "\"} "
         << "1"
         << "\n";

  resp->body = output.str();
  resp->headers["Content-Type"] = "text/plain; version=0.0.4";
  resp->status = 200;
}

#include "htmlfiles.h"

static void serveStuff(HttpRequest* req, HttpResponse* resp)
{
  resp->headers["Cache-Control"] = "max-age=86400";

  if (req->url.path == "/") {
    req->url.path = "/index.html";
  }

  const string charset = "; charset=utf-8";
  if (boost::ends_with(req->url.path, ".html")) {
    resp->headers["Content-Type"] = "text/html" + charset;
  }
  else if (boost::ends_with(req->url.path, ".css")) {
    resp->headers["Content-Type"] = "text/css" + charset;
  }
  else if (boost::ends_with(req->url.path, ".js")) {
    resp->headers["Content-Type"] = "application/javascript" + charset;
  }
  else if (boost::ends_with(req->url.path, ".png")) {
    resp->headers["Content-Type"] = "image/png";
  }

  resp->headers["X-Content-Type-Options"] = "nosniff";
  resp->headers["X-Frame-Options"] = "deny";
  resp->headers["X-Permitted-Cross-Domain-Policies"] = "none";

  resp->headers["X-XSS-Protection"] = "1; mode=block";
  //  resp->headers["Content-Security-Policy"] = "default-src 'self'; style-src 'self' 'unsafe-inline'";

  if (!req->url.path.empty() && (g_urlmap.count(req->url.path.substr(1)) != 0)) {
    resp->body = g_urlmap.at(req->url.path.substr(1));
    resp->status = 200;
  }
  else {
    resp->status = 404;
  }
}

const std::map<std::string, MetricDefinition> MetricDefinitionStorage::d_metrics = {
#include "rec-prometheus-gen.h"
};

constexpr bool CHECK_PROMETHEUS_METRICS = false;

static void validatePrometheusMetrics()
{
  MetricDefinitionStorage s_metricDefinitions;

  auto varmap = getAllStatsMap(StatComponent::API);
  for (const auto& tup : varmap) {
    std::string metricName = tup.first;
    // A few special cases not handled correctly by the check below
    if (metricName.find("cpu-msec-") == 0) {
      continue;
    }
    if (metricName.find("cumul-") == 0) {
      continue;
    }
    if (metricName.find("auth-") == 0 && metricName.find("-answers") != string::npos) {
      continue;
    }
    if (metricName.find("proxy-mapping-total") == 0) {
      continue;
    }
    MetricDefinition metricDetails;

    if (!s_metricDefinitions.getMetricDetails(metricName, metricDetails)) {
      SLOG(g_log << Logger::Debug << "{ \"" << metricName << "\", MetricDefinition(PrometheusMetricType::counter, \"\")}," << endl,
           g_slog->info(Logr::Debug, "{ \"" + metricName + "\", MetricDefinition(PrometheusMetricType::counter, \"\")},"));
    }
  }
}

RecursorWebServer::RecursorWebServer(FDMultiplexer* fdm)
{
  if (CHECK_PROMETHEUS_METRICS) {
    validatePrometheusMetrics();
  }

  d_ws = make_unique<AsyncWebServer>(fdm, arg()["webserver-address"], arg().asNum("webserver-port"));
  d_ws->setSLog(g_slog->withName("webserver"));

  d_ws->setApiKey(arg()["api-key"], arg().mustDo("webserver-hash-plaintext-credentials"));
  d_ws->setPassword(arg()["webserver-password"], arg().mustDo("webserver-hash-plaintext-credentials"));
  d_ws->setLogLevel(arg()["webserver-loglevel"]);

  NetmaskGroup acl;
  acl.toMasks(::arg()["webserver-allow-from"]);
  d_ws->setACL(acl);

  d_ws->bind();

  // legacy dispatch
  d_ws->registerApiHandler(
    "/jsonstat", [](HttpRequest* req, HttpResponse* resp) { jsonstat(req, resp); }, "GET", true);
  d_ws->registerApiHandler("/api/v1/servers/localhost/cache/flush", apiServerCacheFlush, "PUT");
  d_ws->registerApiHandler("/api/v1/servers/localhost/config/allow-from", apiServerConfigAllowFromPUT, "PUT");
  d_ws->registerApiHandler("/api/v1/servers/localhost/config/allow-from", apiServerConfigAllowFromGET, "GET");
  d_ws->registerApiHandler("/api/v1/servers/localhost/config/allow-notify-from", apiServerConfigAllowNotifyFromGET, "GET");
  d_ws->registerApiHandler("/api/v1/servers/localhost/config/allow-notify-from", apiServerConfigAllowNotifyFromPUT, "PUT");
  d_ws->registerApiHandler("/api/v1/servers/localhost/config", apiServerConfig, "GET");
  d_ws->registerApiHandler("/api/v1/servers/localhost/rpzstatistics", apiServerRPZStats, "GET");
  d_ws->registerApiHandler("/api/v1/servers/localhost/search-data", apiServerSearchData, "GET");
  d_ws->registerApiHandler("/api/v1/servers/localhost/statistics", apiServerStatistics, "GET", true);
  d_ws->registerApiHandler("/api/v1/servers/localhost/zones/<id>", apiServerZoneDetailGET, "GET");
  d_ws->registerApiHandler("/api/v1/servers/localhost/zones/<id>", apiServerZoneDetailPUT, "PUT");
  d_ws->registerApiHandler("/api/v1/servers/localhost/zones/<id>", apiServerZoneDetailDELETE, "DELETE");
  d_ws->registerApiHandler("/api/v1/servers/localhost/zones", apiServerZonesGET, "GET");
  d_ws->registerApiHandler("/api/v1/servers/localhost/zones", apiServerZonesPOST, "POST");
  d_ws->registerApiHandler("/api/v1/servers/localhost", apiServerDetail, "GET", true);
  d_ws->registerApiHandler("/api/v1/servers", apiServer, "GET");
  d_ws->registerApiHandler("/api/v1", apiDiscoveryV1, "GET");
  d_ws->registerApiHandler("/api", apiDiscovery, "GET");

  for (const auto& url : g_urlmap) {
    d_ws->registerWebHandler("/" + url.first, serveStuff, "GET");
  }

  d_ws->registerWebHandler("/", serveStuff, "GET");
  d_ws->registerWebHandler("/metrics", prometheusMetrics, "GET");
  d_ws->go();
}

void RecursorWebServer::jsonstat(HttpRequest* req, HttpResponse* resp)
{
  string command;

  if (req->getvars.count("command") != 0) {
    command = req->getvars["command"];
    req->getvars.erase("command");
  }

  map<string, string> stats;
  if (command == "get-query-ring") {
    typedef pair<DNSName, uint16_t> query_t;
    vector<query_t> queries;
    bool filter = !req->getvars["public-filtered"].empty();

    if (req->getvars["name"] == "servfail-queries") {
      queries = broadcastAccFunction<vector<query_t>>(pleaseGetServfailQueryRing);
    }
    else if (req->getvars["name"] == "bogus-queries") {
      queries = broadcastAccFunction<vector<query_t>>(pleaseGetBogusQueryRing);
    }
    else if (req->getvars["name"] == "queries") {
      queries = broadcastAccFunction<vector<query_t>>(pleaseGetQueryRing);
    }

    typedef map<query_t, unsigned int> counts_t;
    counts_t counts;
    for (const query_t& count : queries) {
      if (filter) {
        counts[pair(getRegisteredName(count.first), count.second)]++;
      }
      else {
        counts[pair(count.first, count.second)]++;
      }
    }

    typedef std::multimap<int, query_t> rcounts_t;
    rcounts_t rcounts;

    for (const auto& count : counts) {
      rcounts.emplace(-count.second, count.first);
    }

    Json::array entries;
    unsigned int tot = 0;
    unsigned int totIncluded = 0;
    for (const rcounts_t::value_type& count : rcounts) {
      totIncluded -= count.first;
      entries.push_back(Json::array{
        -count.first, count.second.first.toLogString(), DNSRecordContent::NumberToType(count.second.second)});
      if (tot++ >= 100) {
        break;
      }
    }
    if (queries.size() != totIncluded) {
      entries.push_back(Json::array{
        (int)(queries.size() - totIncluded), "", ""});
    }
    resp->setJsonBody(Json::object{{"entries", entries}});
    return;
  }
  if (command == "get-remote-ring") {
    vector<ComboAddress> queries;
    if (req->getvars["name"] == "remotes") {
      queries = broadcastAccFunction<vector<ComboAddress>>(pleaseGetRemotes);
    }
    else if (req->getvars["name"] == "servfail-remotes") {
      queries = broadcastAccFunction<vector<ComboAddress>>(pleaseGetServfailRemotes);
    }
    else if (req->getvars["name"] == "bogus-remotes") {
      queries = broadcastAccFunction<vector<ComboAddress>>(pleaseGetBogusRemotes);
    }
    else if (req->getvars["name"] == "large-answer-remotes") {
      queries = broadcastAccFunction<vector<ComboAddress>>(pleaseGetLargeAnswerRemotes);
    }
    else if (req->getvars["name"] == "timeouts") {
      queries = broadcastAccFunction<vector<ComboAddress>>(pleaseGetTimeouts);
    }
    typedef map<ComboAddress, unsigned int, ComboAddress::addressOnlyLessThan> counts_t;
    counts_t counts;
    for (const ComboAddress& query : queries) {
      counts[query]++;
    }

    typedef std::multimap<int, ComboAddress> rcounts_t;
    rcounts_t rcounts;

    for (const auto& count : counts) {
      rcounts.emplace(-count.second, count.first);
    }

    Json::array entries;
    unsigned int tot = 0;
    unsigned int totIncluded = 0;
    for (const rcounts_t::value_type& count : rcounts) {
      totIncluded -= count.first;
      entries.push_back(Json::array{
        -count.first, count.second.toString()});
      if (tot++ >= 100) {
        break;
      }
    }
    if (queries.size() != totIncluded) {
      entries.push_back(Json::array{
        (int)(queries.size() - totIncluded), ""});
    }

    resp->setJsonBody(Json::object{{"entries", entries}});
    return;
  }
  resp->setErrorResult("Command '" + command + "' not found", 404);
}

void AsyncServerNewConnectionMT(void* arg)
{
  auto* server = static_cast<AsyncServer*>(arg);

  try {
    auto socket = server->accept(); // this is actually a shared_ptr
    if (socket) {
      server->d_asyncNewConnectionCallback(socket);
    }
  }
  catch (NetworkError& e) {
    // we're running in a shared process/thread, so can't just terminate/abort.
    SLOG(g_log << Logger::Warning << "Network error in web thread: " << e.what() << endl,
         g_slog->withName("webserver")->error(Logr::Warning, e.what(), "Exception in web tread", Logging::Loggable("NetworkError")));
    return;
  }
  catch (...) {
    SLOG(g_log << Logger::Warning << "Unknown error in web thread" << endl,
         g_slog->withName("webserver")->info(Logr::Warning, "Exception in web tread"));

    return;
  }
}

void AsyncServer::asyncWaitForConnections(FDMultiplexer* fdm, const newconnectioncb_t& callback)
{
  d_asyncNewConnectionCallback = callback;
  fdm->addReadFD(d_server_socket.getHandle(), [this](int, boost::any&) { newConnection(); });
}

void AsyncServer::newConnection()
{
  getMT()->makeThread(&AsyncServerNewConnectionMT, this);
}

// This is an entry point from FDM, so it needs to catch everything.
void AsyncWebServer::serveConnection(const std::shared_ptr<Socket>& socket) const // NOLINT(readability-function-cognitive-complexity) #12791 Remove NOLINT(readability-function-cognitive-complexity) omoerbeek
{
  if (!socket->acl(d_acl)) {
    return;
  }

  const auto unique = getUniqueID();
  const string logprefix = d_logprefix + to_string(unique) + " ";

  HttpRequest req(logprefix);
  HttpResponse resp;
#ifdef RECURSOR
  auto log = d_slog->withValues("uniqueid", Logging::Loggable(to_string(unique)));
  req.setSLog(log);
  resp.setSLog(log);
#endif

  ComboAddress remote;
  PacketBuffer reply;

  try {
    YaHTTP::AsyncRequestLoader yarl;
    yarl.initialize(&req);
    socket->setNonBlocking();

    const struct timeval timeout{
      g_networkTimeoutMsec / 1000, static_cast<suseconds_t>(g_networkTimeoutMsec) % 1000 * 1000};
    std::shared_ptr<TLSCtx> tlsCtx{nullptr};
    if (d_loglevel > WebServer::LogLevel::None) {
      socket->getRemote(remote);
    }
    auto handler = std::make_shared<TCPIOHandler>("", false, socket->releaseHandle(), timeout, tlsCtx);

    PacketBuffer data;
    try {
      while (!req.complete) {
        auto ret = arecvtcp(data, 16384, handler, true);
        if (ret == LWResult::Result::Success) {
          string str(reinterpret_cast<const char*>(data.data()), data.size()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): safe cast, data.data() returns unsigned char *
          req.complete = yarl.feed(str);
        }
        else {
          // read error OR EOF
          break;
        }
      }
      yarl.finalize();
    }
    catch (YaHTTP::ParseError& e) {
      // request stays incomplete
      SLOG(g_log << Logger::Warning << logprefix << "Unable to parse request: " << e.what() << endl,
           req.d_slog->error(Logr::Warning, e.what(), "Unable to parse request"));
    }

    if (!validURL(req.url)) {
      throw PDNSException("Received request with invalid URL");
    }
    logRequest(req, remote);

    WebServer::handleRequest(req, resp);
    ostringstream stringStream;
    resp.write(stringStream);
    const string& str = stringStream.str();
    reply.insert(reply.end(), str.cbegin(), str.cend());

    logResponse(resp, remote, logprefix);

    // now send the reply
    if (asendtcp(reply, handler) != LWResult::Result::Success || reply.empty()) {
      SLOG(g_log << Logger::Error << logprefix << "Failed sending reply to HTTP client" << endl,
           req.d_slog->info(Logr::Error, "Failed sending reply to HTTP client"));
    }
    handler->close(); // needed to signal "done" to client
    if (d_loglevel >= WebServer::LogLevel::Normal) {
      SLOG(g_log << Logger::Notice << logprefix << remote << " \"" << req.method << " " << req.url.path << " HTTP/" << req.versionStr(req.version) << "\" " << resp.status << " " << reply.size() << endl,
           req.d_slog->info(Logr::Info, "Request", "remote", Logging::Loggable(remote), "method", Logging::Loggable(req.method),
                            "urlpath", Logging::Loggable(req.url.path), "HTTPVersion", Logging::Loggable(req.versionStr(req.version)),
                            "status", Logging::Loggable(resp.status), "respsize", Logging::Loggable(reply.size())));
    }
  }
  catch (PDNSException& e) {
    SLOG(g_log << Logger::Error << logprefix << "Exception: " << e.reason << endl,
         req.d_slog->error(Logr::Error, e.reason, "Exception handing request", "exception", Logging::Loggable("PDNSException")));
  }
  catch (std::exception& e) {
    if (strstr(e.what(), "timeout") == nullptr) {
      SLOG(g_log << Logger::Error << logprefix << "STL Exception: " << e.what() << endl,
           req.d_slog->error(Logr::Error, e.what(), "Exception handing request", "exception", Logging::Loggable("std::exception")));
    }
  }
  catch (...) {
    SLOG(g_log << Logger::Error << logprefix << "Unknown exception" << endl,
         req.d_slog->error(Logr::Error, "Exception handing request"));
  }
}

void AsyncWebServer::go()
{
  if (!d_server) {
    return;
  }
  auto server = std::dynamic_pointer_cast<AsyncServer>(d_server);
  if (!server) {
    return;
  }
  server->asyncWaitForConnections(d_fdm, [this](const std::shared_ptr<Socket>& socket) { serveConnection(socket); });
}

void serveRustWeb()
{
  ::rust::Vec<::rust::String> urls;
  for (const auto& [url, _] : g_urlmap) {
    urls.emplace_back(url);
  }
  auto address = ComboAddress(arg()["webserver-address"], arg().asNum("webserver-port"));
  pdns::rust::web::rec::serveweb({::rust::String(address.toStringWithPort())}, ::rust::Slice<const ::rust::String>{urls.data(), urls.size()}, arg()["api-key"], arg()["webserver-password"]);
}

static void fromCxxToRust(const HttpResponse& cxxresp, pdns::rust::web::rec::Response& rustResponse)
{
  if (cxxresp.status != 0) {
    rustResponse.status = cxxresp.status;
  }
  rustResponse.body = ::rust::Vec<::rust::u8>();
  rustResponse.body.reserve(cxxresp.body.size());
  std::copy(cxxresp.body.cbegin(), cxxresp.body.cend(), std::back_inserter(rustResponse.body));
  for (const auto& header : cxxresp.headers) {
    rustResponse.headers.emplace_back(pdns::rust::web::rec::KeyValue{header.first, header.second});
  }
}

static void rustWrapper(const std::function<void(HttpRequest*, HttpResponse*)>& func, const pdns::rust::web::rec::Request& rustRequest, pdns::rust::web::rec::Response& rustResponse)
{
  HttpRequest request;
  HttpResponse response;
  request.body = std::string(reinterpret_cast<const char*>(rustRequest.body.data()), rustRequest.body.size());
  request.url = std::string(rustRequest.uri);
  for (const auto& [key, value] : rustRequest.vars) {
    request.getvars[std::string(key)] = std::string(value);
  }
  request.d_slog = g_slog; // XXX
  response.d_slog = g_slog; // XXX
  try {
    func(&request, &response);
  }
  catch (HttpException& e) {
    response.body = e.response().body;
    response.status = e.response().status;
  }
  catch (const ApiException & e) {
    response.setErrorResult(e.what(), 422);
  }
  catch (const JsonException & e) {
    response.setErrorResult(e.what(), 422);
  }
  fromCxxToRust(response, rustResponse);
}

namespace pdns::rust::web::rec
{

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define WRAPPER(A) \
  void A(const Request& rustRequest, Response& rustResponse) { rustWrapper(::A, rustRequest, rustResponse); }

void jsonstat(const Request& rustRequest, Response& rustResponse)
{
  rustWrapper(RecursorWebServer::jsonstat, rustRequest, rustResponse);
}

WRAPPER(apiDiscovery)
WRAPPER(apiDiscoveryV1)
WRAPPER(apiServer)
WRAPPER(apiServerCacheFlush)
WRAPPER(apiServerConfig)
WRAPPER(apiServerConfigAllowFromGET)
WRAPPER(apiServerConfigAllowFromPUT)
WRAPPER(apiServerConfigAllowNotifyFromGET)
WRAPPER(apiServerConfigAllowNotifyFromPUT)
WRAPPER(apiServerDetail)
WRAPPER(apiServerRPZStats)
WRAPPER(apiServerSearchData)
WRAPPER(apiServerStatistics)
WRAPPER(apiServerZoneDetailDELETE)
WRAPPER(apiServerZoneDetailGET)
WRAPPER(apiServerZoneDetailPUT)
WRAPPER(apiServerZonesGET)
WRAPPER(apiServerZonesPOST)
WRAPPER(prometheusMetrics)
WRAPPER(serveStuff)

}
