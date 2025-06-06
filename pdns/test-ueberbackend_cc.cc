#ifndef BOOST_TEST_DYN_LINK
#define BOOST_TEST_DYN_LINK
#endif

#define BOOST_TEST_NO_MAIN

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <unordered_map>

#include <boost/test/unit_test.hpp>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/key_extractors.hpp>

#include "arguments.hh"
#include "auth-querycache.hh"
#include "auth-zonecache.hh"
#include "ueberbackend.hh"

class SimpleBackend : public DNSBackend
{
public:
  struct SimpleDNSRecord
  {
    SimpleDNSRecord(const DNSName& name, uint16_t type, const std::string& content, uint32_t ttl): d_content(content), d_name(name), d_ttl(ttl), d_type(type)
    {
    }

    std::string d_content;
    DNSName d_name;
    uint32_t d_ttl;
    uint16_t d_type;
  };

  struct OrderedNameTypeTag;

  typedef multi_index_container<
    SimpleDNSRecord,
    indexed_by <
      ordered_non_unique<tag<OrderedNameTypeTag>,
                         composite_key<
                           SimpleDNSRecord,
                           member<SimpleDNSRecord, DNSName, &SimpleDNSRecord::d_name>,
                           member<SimpleDNSRecord, uint16_t, &SimpleDNSRecord::d_type>
                           >,
                         composite_key_compare<CanonDNSNameCompare, std::less<uint16_t> >
                         >
      >
    > RecordStorage;

  struct SimpleDNSZone
  {
    SimpleDNSZone(ZoneName name, domainid_t domainId): d_records(std::make_shared<RecordStorage>()), d_name(std::move(name)), d_id(domainId)
    {
    }
    std::shared_ptr<RecordStorage> d_records;
    ZoneName d_name;
    domainid_t d_id;
  };

  struct HashedNameTag {};
  struct IDTag {};

  typedef multi_index_container<
    SimpleDNSZone,
    indexed_by <
      ordered_unique<tag<IDTag>, member<SimpleDNSZone, domainid_t, &SimpleDNSZone::d_id> >,
      hashed_unique<tag<HashedNameTag>, member<SimpleDNSZone, ZoneName, &SimpleDNSZone::d_name> >
      >
    > ZoneStorage;

  struct SimpleMetaData
  {
    SimpleMetaData(ZoneName name, std::string kind, std::vector<std::string> values): d_name(std::move(name)), d_kind(std::move(kind)), d_values(std::move(values))
    {
    }

    ZoneName d_name;
    std::string d_kind;
    std::vector<std::string> d_values;
  };

  struct OrderedNameKindTag {};

  typedef multi_index_container<
    SimpleMetaData,
    indexed_by <
      ordered_unique<tag<OrderedNameKindTag>,
                     composite_key<
                       SimpleMetaData,
                       member<SimpleMetaData, ZoneName, &SimpleMetaData::d_name>,
                       member<SimpleMetaData, std::string, &SimpleMetaData::d_kind>
                       >,
                     composite_key_compare<CanonZoneNameCompare, std::less<> >
                     >
      >
    > MetaDataStorage;

  // Initialize our backend ID from the suffix, skipping the '-' that DNSBackend adds there
  SimpleBackend(const std::string& suffix) :
    d_suffix(suffix), d_backendId(pdns::checked_stoi<domainid_t>(suffix.substr(1)))
  {
  }

  unsigned int getCapabilities() override { return CAP_LIST; }

  bool findZone(const ZoneName& qdomain, domainid_t zoneId, std::shared_ptr<RecordStorage>& records, domainid_t& currentZoneId) const
  {
    currentZoneId = UnknownDomainID;
    records.reset();

    if (zoneId != UnknownDomainID) {
      const auto& idx = boost::multi_index::get<IDTag>(s_zones.at(d_backendId));
      auto it = idx.find(zoneId);
      if (it == idx.end()) {
        return false;
      }
      records = it->d_records;
      currentZoneId = it->d_id;
    }
    else {
      const auto& idx = boost::multi_index::get<HashedNameTag>(s_zones.at(d_backendId));
      auto it = idx.find(qdomain);
      if (it == idx.end()) {
        return false;
      }
      records = it->d_records;
      currentZoneId = it->d_id;
    }

    return true;
  }

  void lookup(const QType& qtype, const DNSName& qdomain, domainid_t zoneId, DNSPacket *pkt_p) override
  {
    d_currentScopeMask = 0;
    findZone(ZoneName(qdomain), zoneId, d_records, d_currentZone);

    if (d_records) {
      if (qdomain == DNSName("geo.powerdns.com.") && pkt_p != nullptr) {
        if (pkt_p->getRealRemote() == Netmask("192.0.2.1")) {
          d_currentScopeMask = 32;
        }
        else if (pkt_p->getRealRemote() == Netmask("198.51.100.1")) {
          d_currentScopeMask = 24;
        }
      }

      auto& idx = d_records->get<OrderedNameTypeTag>();
      if (qtype == QType::ANY) {
        auto range = idx.equal_range(qdomain);
        d_iter = range.first;
        d_end = range.second;
      }
      else {
        auto range = idx.equal_range(std::tuple(qdomain, qtype.getCode()));
        d_iter = range.first;
        d_end = range.second;
      }
    }
  }

  bool get(DNSResourceRecord& drr) override
  {
    if (!d_records) {
      return false;
    }

    if (d_iter == d_end) {
      return false;
    }

    drr.qname = d_iter->d_name;
    drr.domain_id = d_currentZone;
    drr.content = d_iter->d_content;
    drr.qtype = d_iter->d_type;
    drr.ttl = d_iter->d_ttl;

    // drr.auth = d_iter->auth; might bring pain at some point, let's not cross that bridge until then
    drr.auth = true;
    drr.scopeMask = d_currentScopeMask;

    ++d_iter;
    return true;
  }

  bool list(const ZoneName& target, domainid_t zoneId, bool /* include_disabled */) override
  {
    findZone(target, zoneId, d_records, d_currentZone);

    if (d_records) {
      d_iter = d_records->begin();
      d_end = d_records->end();
      return true;
    }

    return false;
  }

  bool getDomainMetadata(const ZoneName& name, const std::string& kind, std::vector<std::string>& meta) override
  {
    const auto& idx = boost::multi_index::get<OrderedNameKindTag>(s_metadata.at(d_backendId));
    auto it = idx.find(std::tuple(name, kind));
    if (it == idx.end()) {
      /* funnily enough, we are expected to return true even though we might not know that zone */
      return true;
    }

    meta = it->d_values;
    return true;
  }

  bool setDomainMetadata(const ZoneName& name, const std::string& kind, const std::vector<std::string>& meta) override
  {
    auto& idx = boost::multi_index::get<OrderedNameKindTag>(s_metadata.at(d_backendId));
    auto it = idx.find(std::tuple(name, kind));
    if (it == idx.end()) {
      s_metadata.at(d_backendId).insert(SimpleMetaData(name, kind, meta));
      return true;
    }
    idx.replace(it, SimpleMetaData(name, kind, meta));
    return true;
  }

  /* this is not thread-safe */
  static std::unordered_map<domainid_t, ZoneStorage> s_zones;
  static std::unordered_map<domainid_t, MetaDataStorage> s_metadata;

protected:
  std::string d_suffix;
  std::shared_ptr<RecordStorage> d_records{nullptr};
  RecordStorage::index<OrderedNameTypeTag>::type::const_iterator d_iter;
  RecordStorage::index<OrderedNameTypeTag>::type::const_iterator d_end;
  const domainid_t d_backendId; // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)
  domainid_t d_currentZone{0}; // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)
  uint8_t d_currentScopeMask{0};
};

class SimpleBackendBestAuth : public SimpleBackend
{
public:
  SimpleBackendBestAuth(const std::string& suffix): SimpleBackend(suffix)
  {
  }

  bool getAuth(const ZoneName& target, SOAData* soadata) override
  {
    static const ZoneName best("d.0.1.0.0.2.ip6.arpa.");

    ++d_authLookupCount;

    if (target.isPartOf(best)) {
      /* return the best SOA right away */
      std::shared_ptr<RecordStorage> records;
      domainid_t zoneId{0};
      if (!findZone(best, UnknownDomainID, records, zoneId)) {
        return false;
      }

      auto& idx = records->get<OrderedNameTypeTag>();
      auto range = idx.equal_range(std::tuple(best.operator const DNSName&(), QType::SOA));
      if (range.first == range.second) {
        return false;
      }

      fillSOAData(range.first->d_content, *soadata);
      soadata->ttl = range.first->d_ttl;
      soadata->zonename = best;
      soadata->domain_id = static_cast<int>(zoneId);
      return true;
    }

    return getSOA(target, UnknownDomainID, *soadata);
  }

  size_t d_authLookupCount{0};
};

class SimpleBackendNoMeta : public SimpleBackend
{
public:
  SimpleBackendNoMeta(const std::string& suffix): SimpleBackend(suffix)
  {
  }

  bool getDomainMetadata(const ZoneName& /* name */, const std::string& /* kind */, std::vector<std::string>& /* meta */) override
  {
    return false;
  }

  bool setDomainMetadata(const ZoneName& /* name */, const std::string& /* kind */, const std::vector<std::string>& /* meta */) override
  {
    return false;
  }
};

std::unordered_map<domainid_t, SimpleBackend::ZoneStorage> SimpleBackend::s_zones;
std::unordered_map<domainid_t, SimpleBackend::MetaDataStorage> SimpleBackend::s_metadata;

class SimpleBackendFactory : public BackendFactory
{
public:
  SimpleBackendFactory(): BackendFactory("SimpleBackend")
  {
  }

  DNSBackend *make(const string& suffix="") override
  {
    return new SimpleBackend(suffix);
  }
};

class SimpleBackendBestAuthFactory : public BackendFactory
{
public:
  SimpleBackendBestAuthFactory(): BackendFactory("SimpleBackendBestAuth")
  {
  }

  DNSBackend *make(const string& suffix="") override
  {
    return new SimpleBackendBestAuth(suffix);
  }
};

class SimpleBackendNoMetaFactory : public BackendFactory
{
public:
  SimpleBackendNoMetaFactory(): BackendFactory("SimpleBackendNoMeta")
  {
  }

  DNSBackend *make(const string& suffix="") override
  {
    return new SimpleBackendNoMeta(suffix);
  }
};

struct UeberBackendSetupArgFixture {
  UeberBackendSetupArgFixture() {
    extern AuthQueryCache QC;
    ::arg().set("query-cache-ttl")="0";
    ::arg().set("negquery-cache-ttl")="0";
    ::arg().set("consistent-backends")="no";
    QC.purge();
    g_zoneCache.setRefreshInterval(0);
    g_zoneCache.clear();
    BackendMakers().clear();
    SimpleBackend::s_zones.clear();
    SimpleBackend::s_metadata.clear();
  };
};

static void testWithoutThenWithAuthCache(std::function<void(UeberBackend& ub)> func)
{
  extern AuthQueryCache QC;

  {
    /* disable the cache */
    ::arg().set("query-cache-ttl")="0";
    ::arg().set("negquery-cache-ttl")="0";
    QC.purge();
    QC.setMaxEntries(0);
    /* keep zone cache disabled */
    g_zoneCache.setRefreshInterval(0);
    g_zoneCache.clear();

    UeberBackend ub;
    func(ub);
  }

  {
    /* enable the cache */
    ::arg().set("query-cache-ttl")="20";
    ::arg().set("negquery-cache-ttl")="60";
    QC.purge();
    QC.setMaxEntries(100000);
    /* keep zone cache disabled */
    g_zoneCache.setRefreshInterval(0);
    g_zoneCache.clear();

    UeberBackend ub;
    /* a first time to fill the cache */
    func(ub);
    /* a second time to make sure every call has been tried with the cache filled */
    func(ub);
  }
}

static void testWithoutThenWithZoneCache(std::function<void(UeberBackend& ub)> func)
{
  extern AuthQueryCache QC;

  {
    /* disable zone cache */
    g_zoneCache.setRefreshInterval(0);
    g_zoneCache.clear();
    /* keep auth caches disabled */
    ::arg().set("query-cache-ttl")="0";
    ::arg().set("negquery-cache-ttl")="0";
    QC.purge();
    QC.setMaxEntries(0);

    UeberBackend ub;
    func(ub);
  }

  //  This test is broken without getAllDomains() in SimpleBackend
  //  {
  //    /* enable zone cache */
  //    //g_zoneCache.setRefreshInterval(60);
  //    g_zoneCache.clear();
  //    /* keep auth caches disabled */
  //    ::arg().set("query-cache-ttl")="0";
  //    ::arg().set("negquery-cache-ttl")="0";
  //    QC.purge();
  //    QC.setMaxEntries(0);
  //
  //    UeberBackend ub;
  //    ub.updateZoneCache();
  //    func(ub);
  //  }
}

BOOST_FIXTURE_TEST_SUITE(test_ueberbackend_cc, UeberBackendSetupArgFixture)

// NOLINTNEXTLINE(readability-identifier-length)
static std::vector<DNSZoneRecord> getRecords(UeberBackend& ub, const DNSName& name, uint16_t qtype, domainid_t zoneId, const DNSPacket* pkt)
{
  std::vector<DNSZoneRecord> result;

  ub.lookup(QType(qtype), name, zoneId, const_cast<DNSPacket*>(pkt));

  DNSZoneRecord dzr;
  while (ub.get(dzr))
  {
    result.push_back(std::move(dzr));
  }

  return result;
}

static void checkRecordExists(const std::vector<DNSZoneRecord>& records, const DNSName& name, uint16_t type, domainid_t zoneId, uint8_t scopeMask, bool auth)
{
  BOOST_REQUIRE_GE(records.size(), 1U);
  for (const auto& record : records) {
    if (record.domain_id == zoneId &&
        record.dr.d_type == type &&
        record.dr.d_name == name &&
        record.auth == auth &&
        record.scopeMask == scopeMask) {
      return;
    }
  }
  BOOST_CHECK_MESSAGE(false, "Record " + name.toString() + "/" + QType(type).toString() + " - " + std::to_string(zoneId) + " not found");
}

BOOST_AUTO_TEST_CASE(test_simple) {

  try {
    SimpleBackend::SimpleDNSZone zoneA(ZoneName("powerdns.com."), 1);
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::SOA, "ns1.powerdns.com. powerdns.com. 3 600 600 3600000 604800", 3600));
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::AAAA, "2001:db8::1", 60));
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("www.powerdns.com."), QType::A, "192.168.0.1", 60));
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("geo.powerdns.com."), QType::A, "192.168.0.42", 60));
    SimpleBackend::s_zones[1].insert(zoneA);

    BackendMakers().report(std::make_unique<SimpleBackendFactory>());
    BackendMakers().launch("SimpleBackend:1");
    UeberBackend::go();

    auto testFunction = [](UeberBackend& ub) -> void {
    {
      // test SOA with unknown zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::SOA, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::SOA, 1, 0, true);
    }

    {
      // test ANY with unknown zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::ANY, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 2U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::SOA, 1, 0, true);
      checkRecordExists(records, DNSName("powerdns.com."), QType::AAAA, 1, 0, true);
    }

    {
      // test AAAA with unknown zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::AAAA, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::AAAA, 1, 0, true);
    }

    {
      // test NODATA with unknown zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::PTR, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 0U);
    }

    {
      // test ANY with zone id set
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::ANY, 1, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 2U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::SOA, 1, 0, true);
      checkRecordExists(records, DNSName("powerdns.com."), QType::AAAA, 1, 0, true);
    }

    {
      // test AAAA with zone id set
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::AAAA, 1, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::AAAA, 1, 0, true);
    }

    {
      // test NODATA with zone id set
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::PTR, 1, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 0U);
    }

    {
      // test ANY with wrong zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::ANY, 65535, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 0U);
    }

    {
      // test a DNS packet is correctly passed and that the corresponding scope is passed back
      DNSPacket pkt(true);
      ComboAddress remote("192.0.2.1");
      pkt.setRemote(&remote);
      auto records = getRecords(ub, DNSName("geo.powerdns.com."), QType::ANY, 1, &pkt);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("geo.powerdns.com."), QType::A, 1, 32, true);
      // and that we don't get the same result for a different client
      remote = ComboAddress("198.51.100.1");
      pkt.setRemote(&remote);
      records = getRecords(ub, DNSName("geo.powerdns.com."), QType::ANY, 1, &pkt);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("geo.powerdns.com."), QType::A, 1, 24, true);
    }

    };
    testWithoutThenWithAuthCache(testFunction);
    testWithoutThenWithZoneCache(testFunction);
  }
  catch(const PDNSException& e) {
    cerr<<e.reason<<endl;
    throw;
  }
  catch(const std::exception& e) {
    cerr<<e.what()<<endl;
    throw;
  }
  catch(...) {
    cerr<<"An unexpected error occurred.."<<endl;
    throw;
  }
}

BOOST_AUTO_TEST_CASE(test_multi_backends_separate_zones) {
  // one zone in backend 1, a second zone in backend 2
  // no overlap

  try {
    SimpleBackend::SimpleDNSZone zoneA(ZoneName("powerdns.com."), 1);
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::SOA, "ns1.powerdns.com. powerdns.com. 3 600 600 3600000 604800", 3600));
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::AAAA, "2001:db8::1", 60));
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("www.powerdns.com."), QType::A, "192.168.0.1", 60));
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("geo.powerdns.com."), QType::A, "192.168.0.42", 60));
    SimpleBackend::s_zones[1].insert(zoneA);

    SimpleBackend::SimpleDNSZone zoneB(ZoneName("powerdns.org."), 2);
    zoneB.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.org."), QType::SOA, "ns1.powerdns.org. powerdns.org. 3 600 600 3600000 604800", 3600));
    zoneB.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.org."), QType::AAAA, "2001:db8::2", 60));
    zoneB.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("www.powerdns.org."), QType::AAAA, "2001:db8::2", 60));
    zoneB.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("geo.powerdns.org."), QType::AAAA, "2001:db8::42", 60));
    SimpleBackend::s_zones[2].insert(zoneB);

    BackendMakers().report(std::make_unique<SimpleBackendFactory>());
    BackendMakers().launch("SimpleBackend:1, SimpleBackend:2");
    UeberBackend::go();

    auto testFunction = [](UeberBackend& ub) -> void {
    {
      // test SOA with unknown zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::SOA, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::SOA, 1, 0, true);

      records = getRecords(ub, DNSName("powerdns.org."), QType::SOA, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("powerdns.org."), QType::SOA, 2, 0, true);
    }

    {
      // test ANY with unknown zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::ANY, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 2U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::SOA, 1, 0, true);
      checkRecordExists(records, DNSName("powerdns.com."), QType::AAAA, 1, 0, true);

      records = getRecords(ub, DNSName("powerdns.org."), QType::ANY, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 2U);
      checkRecordExists(records, DNSName("powerdns.org."), QType::SOA, 2, 0, true);
      checkRecordExists(records, DNSName("powerdns.org."), QType::AAAA, 2, 0, true);
    }

    {
      // test AAAA with unknown zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::AAAA, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::AAAA, 1, 0, true);

      records = getRecords(ub, DNSName("powerdns.org."), QType::AAAA, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("powerdns.org."), QType::AAAA, 2, 0, true);
    }

    {
      // test NODATA with unknown zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::PTR, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 0U);

      records = getRecords(ub, DNSName("powerdns.org."), QType::PTR, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 0U);
    }

    {
      // test ANY with zone id set
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::ANY, 1, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 2U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::SOA, 1, 0, true);
      checkRecordExists(records, DNSName("powerdns.com."), QType::AAAA, 1, 0, true);

      records = getRecords(ub, DNSName("powerdns.org."), QType::ANY, 2, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 2U);
      checkRecordExists(records, DNSName("powerdns.org."), QType::SOA, 2, 0, true);
      checkRecordExists(records, DNSName("powerdns.org."), QType::AAAA, 2, 0, true);
    }

    {
      // test AAAA with zone id set
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::AAAA, 1, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::AAAA, 1, 0, true);

      records = getRecords(ub, DNSName("www.powerdns.org."), QType::AAAA, 2, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("www.powerdns.org."), QType::AAAA, 2, 0, true);
    }

    {
      // test NODATA with zone id set
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::PTR, 1, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 0U);

      records = getRecords(ub, DNSName("powerdns.org."), QType::PTR, 2, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 0U);
    }

    {
      // test ANY with wrong zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::ANY, 2, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 0U);

      records = getRecords(ub, DNSName("powerdns.org."), QType::ANY, 1, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 0U);

      records = getRecords(ub, DNSName("not-powerdns.com."), QType::ANY, 65535, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 0U);
    }

    {
      // test a DNS packet is correctly passed and that the corresponding scope is passed back
      DNSPacket pkt(true);
      ComboAddress remote("192.0.2.1");
      pkt.setRemote(&remote);
      auto records = getRecords(ub, DNSName("geo.powerdns.com."), QType::ANY, 1, &pkt);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("geo.powerdns.com."), QType::A, 1, 32, true);
      // and that we don't get the same result for a different client
      remote = ComboAddress("198.51.100.1");
      pkt.setRemote(&remote);
      records = getRecords(ub, DNSName("geo.powerdns.com."), QType::ANY, 1, &pkt);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("geo.powerdns.com."), QType::A, 1, 24, true);
    }

    };
    testWithoutThenWithAuthCache(testFunction);
    testWithoutThenWithZoneCache(testFunction);
  }
  catch(const PDNSException& e) {
    cerr<<e.reason<<endl;
    throw;
  }
  catch(const std::exception& e) {
    cerr<<e.what()<<endl;
    throw;
  }
  catch(...) {
    cerr<<"An unexpected error occurred.."<<endl;
    throw;
  }
}

BOOST_AUTO_TEST_CASE(test_multi_backends_overlay) {
  // one backend holds the SOA, NS and one A
  // a second backend holds another A and AAAA
  try {
    SimpleBackend::SimpleDNSZone zoneA(ZoneName("powerdns.com."), 1);
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::SOA, "ns1.powerdns.com. powerdns.com. 3 600 600 3600000 604800", 3600));
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::NS, "ns1.powerdns.com.", 3600));
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::A, "192.168.0.1", 60));
    SimpleBackend::s_zones[1].insert(zoneA);

    SimpleBackend::SimpleDNSZone zoneB(ZoneName("powerdns.com."), 1);
    zoneB.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::A, "192.168.0.2", 60));
    zoneB.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::AAAA, "2001:db8::1", 60));
    zoneB.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("www.powerdns.com."), QType::A, "192.168.0.1", 60));
    zoneB.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("geo.powerdns.com."), QType::A, "192.168.0.42", 60));
    SimpleBackend::s_zones[2].insert(zoneB);

    BackendMakers().report(std::make_unique<SimpleBackendFactory>());
    BackendMakers().launch("SimpleBackend:1, SimpleBackend:2");
    UeberBackend::go();

    auto testFunction = [](UeberBackend& ub) -> void {
    {
      // test SOA with unknown zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::SOA, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::SOA, 1, 0, true);
    }

    {
      // test ANY with unknown zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::ANY, UnknownDomainID, nullptr);
      // /!\ only 3 records are returned since we don't allow spreading the same name over several backends
      BOOST_REQUIRE_EQUAL(records.size(), 3U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::SOA, 1, 0, true);
      checkRecordExists(records, DNSName("powerdns.com."), QType::NS, 1, 0, true);
      checkRecordExists(records, DNSName("powerdns.com."), QType::A, 1, 0, true);
      //checkRecordExists(records, DNSName("powerdns.com."), QType::AAAA, 1, 0, true);
    }

    {
      // test AAAA with unknown zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::AAAA, UnknownDomainID, nullptr);
      // /!\ the AAAA will be found on an exact search, but not on an ANY one
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::AAAA, 1, 0, true);
    }

    {
      // test NODATA with unknown zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::PTR, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 0U);
    }

    {
      // test ANY with zone id set
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::ANY, 1, nullptr);
      // /!\ only 3 records are returned since we don't allow spreading the same name over several backends
      BOOST_REQUIRE_EQUAL(records.size(), 3U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::SOA, 1, 0, true);
      checkRecordExists(records, DNSName("powerdns.com."), QType::NS, 1, 0, true);
      checkRecordExists(records, DNSName("powerdns.com."), QType::A, 1, 0, true);
      //checkRecordExists(records, DNSName("powerdns.com."), QType::AAAA, 1, 0, true);
    }

    {
      // test AAAA with zone id set
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::AAAA, 1, nullptr);
      // /!\ the AAAA will be found on an exact search, but not on an ANY one
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::AAAA, 1, 0, true);
    }

    {
      // test www - A with zone id set (only in the second backend)
      auto records = getRecords(ub, DNSName("www.powerdns.com."), QType::A, 1, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("www.powerdns.com."), QType::A, 1, 0, true);
    }

    {
      // test NODATA with zone id set
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::PTR, 1, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 0U);
    }

    {
      // test ANY with wrong zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::ANY, 2, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 0U);
    }

    {
      // test a DNS packet is correctly passed and that the corresponding scope is passed back
      DNSPacket pkt(true);
      ComboAddress remote("192.0.2.1");
      pkt.setRemote(&remote);
      auto records = getRecords(ub, DNSName("geo.powerdns.com."), QType::ANY, 1, &pkt);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("geo.powerdns.com."), QType::A, 1, 32, true);
      // and that we don't get the same result for a different client
      remote = ComboAddress("198.51.100.1");
      pkt.setRemote(&remote);
      records = getRecords(ub, DNSName("geo.powerdns.com."), QType::ANY, 1, &pkt);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("geo.powerdns.com."), QType::A, 1, 24, true);
    }

    };
    testWithoutThenWithAuthCache(testFunction);
    testWithoutThenWithZoneCache(testFunction);
  }
  catch(const PDNSException& e) {
    cerr<<e.reason<<endl;
    throw;
  }
  catch(const std::exception& e) {
    cerr<<e.what()<<endl;
    throw;
  }
  catch(...) {
    cerr<<"An unexpected error occurred.."<<endl;
    throw;
  }
}

BOOST_AUTO_TEST_CASE(test_multi_backends_overlay_name) {
  // one backend holds the apex with SOA, NS and one A
  // a second backend holds others names
  try {
    SimpleBackend::SimpleDNSZone zoneA(ZoneName("powerdns.com."), 1);
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::SOA, "ns1.powerdns.com. powerdns.com. 3 600 600 3600000 604800", 3600));
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::NS, "ns1.powerdns.com.", 3600));
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::A, "192.168.0.1", 60));
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::A, "192.168.0.2", 60));
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::AAAA, "2001:db8::1", 60));
    SimpleBackend::s_zones[1].insert(zoneA);

    SimpleBackend::SimpleDNSZone zoneB(ZoneName("powerdns.com."), 1);
    zoneB.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("www.powerdns.com."), QType::A, "192.168.0.1", 60));
    zoneB.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("www.powerdns.com."), QType::AAAA, "192.168.0.1", 60));
    zoneB.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("geo.powerdns.com."), QType::A, "192.168.0.42", 60));
    SimpleBackend::s_zones[2].insert(zoneB);

    BackendMakers().report(std::make_unique<SimpleBackendFactory>());
    BackendMakers().launch("SimpleBackend:1, SimpleBackend:2");
    UeberBackend::go();

    auto testFunction = [](UeberBackend& ub) -> void {
    {
      // test SOA with unknown zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::SOA, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::SOA, 1, 0, true);
    }

    {
      // test ANY with unknown zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::ANY, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 5U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::SOA, 1, 0, true);
      checkRecordExists(records, DNSName("powerdns.com."), QType::NS, 1, 0, true);
      checkRecordExists(records, DNSName("powerdns.com."), QType::A, 1, 0, true);
      checkRecordExists(records, DNSName("powerdns.com."), QType::AAAA, 1, 0, true);
    }

    {
      // test AAAA with unknown zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::AAAA, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::AAAA, 1, 0, true);
    }

    {
      // test NODATA with unknown zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::PTR, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 0U);
    }

    {
      // test ANY with zone id set
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::ANY, 1, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 5U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::SOA, 1, 0, true);
      checkRecordExists(records, DNSName("powerdns.com."), QType::NS, 1, 0, true);
      checkRecordExists(records, DNSName("powerdns.com."), QType::A, 1, 0, true);
      checkRecordExists(records, DNSName("powerdns.com."), QType::AAAA, 1, 0, true);
    }

    {
      // test AAAA with zone id set
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::AAAA, 1, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::AAAA, 1, 0, true);
    }

    {
      // test www - A with zone id set (only in the second backend)
      auto records = getRecords(ub, DNSName("www.powerdns.com."), QType::A, 1, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("www.powerdns.com."), QType::A, 1, 0, true);
    }

    {
      // test NODATA with zone id set
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::PTR, 1, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 0U);
    }

    {
      // test ANY with wrong zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::ANY, 2, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 0U);
    }

    {
      // test a DNS packet is correctly passed and that the corresponding scope is passed back
      DNSPacket pkt(true);
      ComboAddress remote("192.0.2.1");
      pkt.setRemote(&remote);
      auto records = getRecords(ub, DNSName("geo.powerdns.com."), QType::ANY, 1, &pkt);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("geo.powerdns.com."), QType::A, 1, 32, true);
      // and that we don't get the same result for a different client
      remote = ComboAddress("198.51.100.1");
      pkt.setRemote(&remote);
      records = getRecords(ub, DNSName("geo.powerdns.com."), QType::ANY, 1, &pkt);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("geo.powerdns.com."), QType::A, 1, 24, true);
    }

    };
    testWithoutThenWithAuthCache(testFunction);
    testWithoutThenWithZoneCache(testFunction);
  }
  catch(const PDNSException& e) {
    cerr<<e.reason<<endl;
    throw;
  }
  catch(const std::exception& e) {
    cerr<<e.what()<<endl;
    throw;
  }
  catch(...) {
    cerr<<"An unexpected error occurred.."<<endl;
    throw;
  }
}

BOOST_AUTO_TEST_CASE(test_child_zone) {
  // Backend 1 holds zone A "com" while backend 2 holds zone B "powerdns.com"
  // Check that DS queries are correctly handled

  try {
    SimpleBackend::SimpleDNSZone zoneA(ZoneName("com."), 1);
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("com."), QType::SOA, "a.gtld-servers.net. nstld.verisign-grs.com. 3 600 600 3600000 604800", 3600));
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::NS, "ns1.powerdns.com.", 3600));
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::DS, "44030 8 3 7DD75AE1565051F9563CF8DF976AC99CDCA51E3463019C81BD2BB083 82F3854E", 3600));
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("ns1.powerdns.com."), QType::A, "192.0.2.1", 3600));
    SimpleBackend::s_zones[1].insert(zoneA);

    SimpleBackend::SimpleDNSZone zoneB(ZoneName("powerdns.com."), 2);
    zoneB.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::SOA, "ns1.powerdns.com. powerdns.com. 3 600 600 3600000 604800", 3600));
    zoneB.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::AAAA, "2001:db8::2", 60));
    zoneB.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::NS, "ns1.powerdns.com.", 3600));
    zoneB.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("ns1.powerdns.com."), QType::A, "192.0.2.1", 3600));
    SimpleBackend::s_zones[2].insert(zoneB);

    BackendMakers().report(std::make_unique<SimpleBackendFactory>());
    BackendMakers().launch("SimpleBackend:1, SimpleBackend:2");
    UeberBackend::go();

    auto testFunction = [](UeberBackend& ub) -> void {
    {
      // test SOA with unknown zone id
      auto records = getRecords(ub, DNSName("com."), QType::SOA, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("com."), QType::SOA, 1, 0, true);

      records = getRecords(ub, DNSName("powerdns.com."), QType::SOA, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::SOA, 2, 0, true);
    }

    {
      // test ANY with unknown zone id
      auto records = getRecords(ub, DNSName("powerdns.com."), QType::ANY, UnknownDomainID, nullptr);
      BOOST_REQUIRE_EQUAL(records.size(), 3U);
      checkRecordExists(records, DNSName("powerdns.com."), QType::SOA, 2, 0, true);
      checkRecordExists(records, DNSName("powerdns.com."), QType::NS, 2, 0, true);
      checkRecordExists(records, DNSName("powerdns.com."), QType::AAAA, 2, 0, true);
    }

    {
      // test getAuth() for DS
      SOAData sd;
      BOOST_REQUIRE(ub.getAuth(ZoneName("powerdns.com."), QType::DS, &sd, Netmask{}));
      BOOST_CHECK_EQUAL(sd.zonename.toString(), "com.");
      BOOST_CHECK_EQUAL(sd.domain_id, 1);
    }

    {
      // test getAuth() for A
      SOAData sd;
      BOOST_REQUIRE(ub.getAuth(ZoneName("powerdns.com."), QType::A, &sd, Netmask{}));
      BOOST_CHECK_EQUAL(sd.zonename.toString(), "powerdns.com.");
      BOOST_CHECK_EQUAL(sd.domain_id, 2);
    }

    };
    testWithoutThenWithAuthCache(testFunction);
    testWithoutThenWithZoneCache(testFunction);
  }
  catch(const PDNSException& e) {
    cerr<<e.reason<<endl;
    throw;
  }
  catch(const std::exception& e) {
    cerr<<e.what()<<endl;
    throw;
  }
  catch(...) {
    cerr<<"An unexpected error occurred.."<<endl;
    throw;
  }
}

BOOST_AUTO_TEST_CASE(test_multi_backends_best_soa) {
  // several backends, one returns the best SOA it has right away
  // while the others do simple lookups

  try {
    SimpleBackend::SimpleDNSZone zoneA(ZoneName("d.0.1.0.0.2.ip6.arpa."), 1);
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("d.0.1.0.0.2.ip6.arpa."), QType::SOA, "ns.apnic.net. read-txt-record-of-zone-first-dns-admin.apnic.net. 3005126844 7200 1800 604800 3600", 3600));
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("2.4.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.8.b.d.0.1.0.0.2.ip6.arpa."), QType::PTR, "a.reverse.", 3600));
    SimpleBackend::s_zones[1].insert(zoneA);

    SimpleBackend::SimpleDNSZone zoneB(ZoneName("0.1.0.0.2.ip6.arpa."), 2);
    zoneB.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("0.1.0.0.2.ip6.arpa."), QType::SOA, "ns.apnic.net. read-txt-record-of-zone-first-dns-admin.apnic.net. 3005126844 7200 1800 604800 3600", 3600));
    SimpleBackend::s_zones[2].insert(zoneB);

    BackendMakers().report(std::make_unique<SimpleBackendFactory>());
    BackendMakers().report(std::make_unique<SimpleBackendBestAuthFactory>());
    BackendMakers().launch("SimpleBackendBestAuth:1, SimpleBackend:2");
    UeberBackend::go();

    auto testFunction = [](UeberBackend& ub) -> void {
    {
      auto* sbba = dynamic_cast<SimpleBackendBestAuth*>(ub.backends.at(0).get());
      BOOST_REQUIRE(sbba != nullptr);

      // NOLINTNEXTLINE (clang-analyzer-core.NullDereference): Not sure.
      sbba->d_authLookupCount = 0;

      // test getAuth()
      SOAData sd;
      BOOST_REQUIRE(ub.getAuth(ZoneName("2.4.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.8.b.d.0.1.0.0.2.ip6.arpa."), QType::PTR, &sd, Netmask{}));
      BOOST_CHECK_EQUAL(sd.zonename.toString(), "d.0.1.0.0.2.ip6.arpa.");
      BOOST_CHECK_EQUAL(sd.domain_id, 1);

      // check that at most one auth lookup occurred to this backend (O with caching enabled)
      BOOST_CHECK_LE(sbba->d_authLookupCount, 1U);
    }

    };
    testWithoutThenWithAuthCache(testFunction);
    testWithoutThenWithZoneCache(testFunction);
  }
  catch(const PDNSException& e) {
    cerr<<e.reason<<endl;
    throw;
  }
  catch(const std::exception& e) {
    cerr<<e.what()<<endl;
    throw;
  }
  catch(...) {
    cerr<<"An unexpected error occurred.."<<endl;
    throw;
  }
}

BOOST_AUTO_TEST_CASE(test_multi_backends_metadata) {
  // we have metadata stored in the first and second backend.
  // We can read from the first backend but not from the second, since the first will return "true" even though it has nothing
  // Updating will insert into the first backend, leaving the first one untouched

  try {
    SimpleBackend::SimpleDNSZone zoneA(ZoneName("powerdns.com."), 1);
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::SOA, "ns1.powerdns.com. powerdns.com. 3 600 600 3600000 604800", 3600));
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.com."), QType::AAAA, "2001:db8::1", 60));
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("www.powerdns.com."), QType::A, "192.168.0.1", 60));
    zoneA.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("geo.powerdns.com."), QType::A, "192.168.0.42", 60));
    SimpleBackend::s_zones[1].insert(zoneA);
    SimpleBackend::s_metadata[1].insert(SimpleBackend::SimpleMetaData(ZoneName("powerdns.com."), "test-data-a", { "value1", "value2"}));

    SimpleBackend::SimpleDNSZone zoneB(ZoneName("powerdns.org."), 2);
    zoneB.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.org."), QType::SOA, "ns1.powerdns.org. powerdns.org. 3 600 600 3600000 604800", 3600));
    zoneB.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("powerdns.org."), QType::AAAA, "2001:db8::2", 60));
    zoneB.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("www.powerdns.org."), QType::AAAA, "2001:db8::2", 60));
    zoneB.d_records->insert(SimpleBackend::SimpleDNSRecord(DNSName("geo.powerdns.org."), QType::AAAA, "2001:db8::42", 60));
    SimpleBackend::s_zones[2].insert(zoneB);
    SimpleBackend::s_metadata[2].insert(SimpleBackend::SimpleMetaData(ZoneName("powerdns.org."), "test-data-b", { "value1", "value2"}));

    BackendMakers().report(std::make_unique<SimpleBackendFactory>());
    BackendMakers().launch("SimpleBackend:1, SimpleBackend:2");
    UeberBackend::go();

    auto testFunction = [](UeberBackend& ub) -> void {
    {
      // check the initial values
      std::vector<std::string> values;
      BOOST_CHECK(ub.getDomainMetadata(ZoneName("powerdns.com."), "test-data-a", values));
      BOOST_REQUIRE_EQUAL(values.size(), 2U);
      BOOST_CHECK_EQUAL(values.at(0), "value1");
      BOOST_CHECK_EQUAL(values.at(1), "value2");
      values.clear();
      BOOST_CHECK(ub.getDomainMetadata(ZoneName("powerdns.com."), "test-data-b", values));
      BOOST_CHECK_EQUAL(values.size(), 0U);
      values.clear();
      BOOST_CHECK(ub.getDomainMetadata(ZoneName("powerdns.org."), "test-data-a", values));
      BOOST_CHECK_EQUAL(values.size(), 0U);
      values.clear();
      BOOST_CHECK(ub.getDomainMetadata(ZoneName("powerdns.org."), "test-data-b", values));
      BOOST_CHECK_EQUAL(values.size(), 0U);
    }

    {
      // update the values
      BOOST_CHECK(ub.setDomainMetadata(ZoneName("powerdns.com."), "test-data-a", std::vector<std::string>({"value3"})));
      BOOST_CHECK(ub.setDomainMetadata(ZoneName("powerdns.org."), "test-data-a", std::vector<std::string>({"value4"})));
      BOOST_CHECK(ub.setDomainMetadata(ZoneName("powerdns.org."), "test-data-b", std::vector<std::string>({"value5"})));
    }

    // check the updated values
    {
      std::vector<std::string> values;
      BOOST_CHECK(ub.getDomainMetadata(ZoneName("powerdns.com."), "test-data-a", values));
      BOOST_REQUIRE_EQUAL(values.size(), 1U);
      BOOST_CHECK_EQUAL(values.at(0), "value3");
      values.clear();
      BOOST_CHECK(ub.getDomainMetadata(ZoneName("powerdns.org."), "test-data-a", values));
      BOOST_REQUIRE_EQUAL(values.size(), 1U);
      BOOST_CHECK_EQUAL(values.at(0), "value4");
      values.clear();
      BOOST_CHECK(ub.getDomainMetadata(ZoneName("powerdns.org."), "test-data-b", values));
      BOOST_REQUIRE_EQUAL(values.size(), 1U);
      BOOST_CHECK_EQUAL(values.at(0), "value5");
    }

    {
      // check that it has not been updated in the second backend
      const auto& iter = SimpleBackend::s_metadata[2].find(std::tuple(ZoneName("powerdns.org."), "test-data-b"));
      BOOST_REQUIRE(iter != SimpleBackend::s_metadata[2].end());
      BOOST_REQUIRE_EQUAL(iter->d_values.size(), 2U);
      BOOST_CHECK_EQUAL(iter->d_values.at(0), "value1");
      BOOST_CHECK_EQUAL(iter->d_values.at(1), "value2");
    }
    };

    UeberBackend ub;
    testFunction(ub);
  }
  catch(const PDNSException& e) {
    cerr<<e.reason<<endl;
    throw;
  }
  catch(const std::exception& e) {
    cerr<<e.what()<<endl;
    throw;
  }
  catch(...) {
    cerr<<"An unexpected error occurred.."<<endl;
    throw;
  }
}


BOOST_AUTO_TEST_SUITE_END();
