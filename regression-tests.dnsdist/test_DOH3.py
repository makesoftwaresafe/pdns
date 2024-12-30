#!/usr/bin/env python
import dns
import clientsubnetoption

from dnsdisttests import DNSDistTest
from dnsdisttests import pickAvailablePort
from quictests import QUICTests, QUICWithCacheTests, QUICACLTests, QUICGetLocalAddressOnAnyBindTests, QUICXFRTests
import doh3client

class TestDOH3(QUICTests, DNSDistTest):
    _serverKey = 'server.key'
    _serverCert = 'server.chain'
    _serverName = 'tls.tests.dnsdist.org'
    _caCert = 'ca.pem'
    _doqServerPort = pickAvailablePort()
    _dohBaseURL = ("https://%s:%d/" % (_serverName, _doqServerPort))
    _config_template = """
    newServer{address="127.0.0.1:%d"}

    addAction("drop.doq.tests.powerdns.com.", DropAction())
    addAction("refused.doq.tests.powerdns.com.", RCodeAction(DNSRCode.REFUSED))
    addAction("spoof.doq.tests.powerdns.com.", SpoofAction("1.2.3.4"))
    addAction(HTTPHeaderRule("X-PowerDNS", "^[a]{5}$"), SpoofAction("2.3.4.5"))
    addAction(HTTPPathRule("/PowerDNS"), SpoofAction("3.4.5.6"))
    addAction(HTTPPathRegexRule("^/PowerDNS-[0-9]"), SpoofAction("6.7.8.9"))
    addAction("no-backend.doq.tests.powerdns.com.", PoolAction('this-pool-has-no-backend'))

    function dohHandler(dq)
      if dq:getHTTPScheme() == 'https' and dq:getHTTPHost() == '%s:%d' and dq:getHTTPPath() == '/' and dq:getHTTPQueryString() == '' then
        local foundct = false
        for key,value in pairs(dq:getHTTPHeaders()) do
          if key == 'content-type' and value == 'application/dns-message' then
            foundct = true
            break
          end
        end
        if foundct then
          return DNSAction.Spoof, "10.11.12.13"
        end
      end
      return DNSAction.None
    end
    addAction("http-lua.doh3.tests.powerdns.com.", LuaAction(dohHandler))

    addDOH3Local("127.0.0.1:%d", "%s", "%s", {keyLogFile='/tmp/keys'})
    """
    _config_params = ['_testServerPort',  '_serverName', '_doqServerPort', '_doqServerPort','_serverCert', '_serverKey']
    _verboseMode = True

    def getQUICConnection(self):
        return self.getDOQConnection(self._doqServerPort, self._caCert)

    def sendQUICQuery(self, query, response=None, useQueue=True, connection=None):
        return self.sendDOH3Query(self._doqServerPort, self._dohBaseURL, query, response=response, caFile=self._caCert, useQueue=useQueue, serverName=self._serverName, connection=connection)

    def testHeaderRule(self):
        """
        DOH3: HeaderRule
        """
        name = 'header-rule.doh3.tests.powerdns.com.'
        query = dns.message.make_query(name, 'A', 'IN')
        query.id = 0
        query.flags &= ~dns.flags.RD
        expectedResponse = dns.message.make_response(query)
        rrset = dns.rrset.from_text(name,
                                    3600,
                                    dns.rdataclass.IN,
                                    dns.rdatatype.A,
                                    '2.3.4.5')
        expectedResponse.answer.append(rrset)

        # this header should match
        (_, receivedResponse) = self.sendDOH3Query(self._doqServerPort, self._dohBaseURL, query=query, response=None, useQueue=False, caFile=self._caCert, customHeaders={'x-powerdnS': 'aaaaa'})
        self.assertEqual(receivedResponse, expectedResponse)

        expectedQuery = dns.message.make_query(name, 'A', 'IN', use_edns=True, payload=4096)
        expectedQuery.flags &= ~dns.flags.RD
        expectedQuery.id = 0
        response = dns.message.make_response(query)
        rrset = dns.rrset.from_text(name,
                                    3600,
                                    dns.rdataclass.IN,
                                    dns.rdatatype.A,
                                    '127.0.0.1')
        response.answer.append(rrset)

        # this content of the header should NOT match
        (receivedQuery, receivedResponse) = self.sendDOH3Query(self._doqServerPort, self._dohBaseURL, query, response=response, caFile=self._caCert, customHeaders={'x-powerdnS': 'bbbbb'})
        self.assertTrue(receivedQuery)
        self.assertTrue(receivedResponse)
        receivedQuery.id = expectedQuery.id
        self.assertEqual(expectedQuery, receivedQuery)
        self.checkQueryNoEDNS(expectedQuery, receivedQuery)
        self.assertEqual(response, receivedResponse)

    def testHTTPPath(self):
        """
        DOH3: HTTPPath
        """
        name = 'http-path.doh3.tests.powerdns.com.'
        query = dns.message.make_query(name, 'A', 'IN')
        query.id = 0
        query.flags &= ~dns.flags.RD
        expectedResponse = dns.message.make_response(query)
        rrset = dns.rrset.from_text(name,
                                    3600,
                                    dns.rdataclass.IN,
                                    dns.rdatatype.A,
                                    '3.4.5.6')
        expectedResponse.answer.append(rrset)

        # this path should match
        (_, receivedResponse) = self.sendDOH3Query(self._doqServerPort, self._dohBaseURL + 'PowerDNS', caFile=self._caCert, query=query, response=None, useQueue=False)
        self.assertEqual(receivedResponse, expectedResponse)

        expectedQuery = dns.message.make_query(name, 'A', 'IN')
        expectedQuery.id = 0
        expectedQuery.flags &= ~dns.flags.RD
        response = dns.message.make_response(query)
        rrset = dns.rrset.from_text(name,
                                    3600,
                                    dns.rdataclass.IN,
                                    dns.rdatatype.A,
                                    '127.0.0.1')
        response.answer.append(rrset)

        # this path should NOT match
        (receivedQuery, receivedResponse) = self.sendDOH3Query(self._doqServerPort, self._dohBaseURL + "PowerDNS2", query, response=response, caFile=self._caCert)
        self.assertTrue(receivedQuery)
        self.assertTrue(receivedResponse)
        receivedQuery.id = expectedQuery.id
        self.assertEqual(expectedQuery, receivedQuery)
        self.checkQueryNoEDNS(expectedQuery, receivedQuery)
        self.assertEqual(response, receivedResponse)

    def testHTTPPathRegex(self):
        """
        DOH3: HTTPPathRegex
        """
        name = 'http-path-regex.doh3.tests.powerdns.com.'
        query = dns.message.make_query(name, 'A', 'IN')
        query.id = 0
        query.flags &= ~dns.flags.RD
        expectedResponse = dns.message.make_response(query)
        rrset = dns.rrset.from_text(name,
                                    3600,
                                    dns.rdataclass.IN,
                                    dns.rdatatype.A,
                                    '6.7.8.9')
        expectedResponse.answer.append(rrset)

        # this path should match
        (_, receivedResponse) = self.sendDOH3Query(self._doqServerPort, self._dohBaseURL + 'PowerDNS-999', caFile=self._caCert, query=query, response=None, useQueue=False)
        self.assertEqual(receivedResponse, expectedResponse)

        expectedQuery = dns.message.make_query(name, 'A', 'IN')
        expectedQuery.id = 0
        expectedQuery.flags &= ~dns.flags.RD
        response = dns.message.make_response(query)
        rrset = dns.rrset.from_text(name,
                                    3600,
                                    dns.rdataclass.IN,
                                    dns.rdatatype.A,
                                    '127.0.0.1')
        response.answer.append(rrset)

        # this path should NOT match
        (receivedQuery, receivedResponse) = self.sendDOH3Query(self._doqServerPort, self._dohBaseURL + "PowerDNS2", query, response=response, caFile=self._caCert)
        self.assertTrue(receivedQuery)
        self.assertTrue(receivedResponse)
        receivedQuery.id = expectedQuery.id
        self.assertEqual(expectedQuery, receivedQuery)
        self.checkQueryNoEDNS(expectedQuery, receivedQuery)
        self.assertEqual(response, receivedResponse)

    def testHTTPLuaBindings(self):
        """
        DOH3: Lua HTTP bindings
        """
        name = 'http-lua.doh3.tests.powerdns.com.'
        query = dns.message.make_query(name, 'A', 'IN', use_edns=False)
        query.id = 0

        (_, receivedResponse) = self.sendDOH3Query(self._doqServerPort, self._dohBaseURL, query, caFile=self._caCert, useQueue=False, post=True)
        self.assertTrue(receivedResponse)

class TestDOH3ACL(QUICACLTests, DNSDistTest):
    _serverKey = 'server.key'
    _serverCert = 'server.chain'
    _serverName = 'tls.tests.dnsdist.org'
    _caCert = 'ca.pem'
    _doqServerPort = pickAvailablePort()
    _dohBaseURL = ("https://%s:%d/" % (_serverName, _doqServerPort))
    _config_template = """
    newServer{address="127.0.0.1:%d"}

    setACL("192.0.2.1/32")
    addDOH3Local("127.0.0.1:%d", "%s", "%s", {keyLogFile='/tmp/keys'})
    """
    _config_params = ['_testServerPort', '_doqServerPort','_serverCert', '_serverKey']
    _verboseMode = True

    def getQUICConnection(self):
        return self.getDOQConnection(self._doqServerPort, self._caCert)

    def sendQUICQuery(self, query, response=None, useQueue=True, connection=None):
        return self.sendDOH3Query(self._doqServerPort, self._dohBaseURL, query, response=response, caFile=self._caCert, useQueue=useQueue, serverName=self._serverName, connection=connection)

class TestDOH3Specifics(DNSDistTest):
    _serverKey = 'server.key'
    _serverCert = 'server.chain'
    _serverName = 'tls.tests.dnsdist.org'
    _caCert = 'ca.pem'
    _doqServerPort = pickAvailablePort()
    _dohBaseURL = ("https://%s:%d/" % (_serverName, _doqServerPort))
    _config_template = """
    newServer{address="127.0.0.1:%d"}

    addDOH3Local("127.0.0.1:%d", "%s", "%s", {keyLogFile='/tmp/keys'})
    """
    _config_params = ['_testServerPort', '_doqServerPort','_serverCert', '_serverKey']
    _verboseMode = True

    def testDOH3Post(self):
        """
        QUIC: Simple POST query
        """
        name = 'simple.post.doq.tests.powerdns.com.'
        query = dns.message.make_query(name, 'A', 'IN', use_edns=False)
        query.id = 0
        expectedQuery = dns.message.make_query(name, 'A', 'IN', use_edns=True, payload=4096)
        expectedQuery.id = 0
        response = dns.message.make_response(query)
        rrset = dns.rrset.from_text(name,
                                    3600,
                                    dns.rdataclass.IN,
                                    dns.rdatatype.A,
                                    '127.0.0.1')
        response.answer.append(rrset)
        (receivedQuery, receivedResponse) = self.sendDOH3Query(self._doqServerPort, self._dohBaseURL, query, response=response, caFile=self._caCert, serverName=self._serverName, post=True)
        self.assertTrue(receivedQuery)
        self.assertTrue(receivedResponse)
        receivedQuery.id = expectedQuery.id
        self.assertEqual(expectedQuery, receivedQuery)
        self.assertEqual(receivedResponse, response)

class TestDOH3GetLocalAddressOnAnyBind(QUICGetLocalAddressOnAnyBindTests, DNSDistTest):
    _serverKey = 'server.key'
    _serverCert = 'server.chain'
    _serverName = 'tls.tests.dnsdist.org'
    _caCert = 'ca.pem'
    _doqServerPort = pickAvailablePort()
    _dohBaseURL = ("https://%s:%d/" % (_serverName, _doqServerPort))
    _config_template = """
    function answerBasedOnLocalAddress(dq)
      local dest = tostring(dq.localaddr)
      local i, j = string.find(dest, "[0-9.]+")
      local addr = string.sub(dest, i, j)
      local dashAddr = string.gsub(addr, "[.]", "-")
      return DNSAction.Spoof, "address-was-"..dashAddr..".local-address-any.advanced.tests.powerdns.com."
    end
    addAction("local-address-any.quic.tests.powerdns.com.", LuaAction(answerBasedOnLocalAddress))
    newServer{address="127.0.0.1:%s"}
    addDOH3Local("0.0.0.0:%d", "%s", "%s")
    addDOH3Local("[::]:%d", "%s", "%s")
    """
    _config_params = ['_testServerPort', '_doqServerPort','_serverCert', '_serverKey', '_doqServerPort','_serverCert', '_serverKey']
    _acl = ['127.0.0.1/32', '::1/128']
    _skipListeningOnCL = True

    def getQUICConnection(self):
        return self.getDOQConnection(self._doqServerPort, self._caCert)

    def sendQUICQuery(self, query, response=None, useQueue=True, connection=None):
        return self.sendDOH3Query(self._doqServerPort, self._dohBaseURL, query, response=response, caFile=self._caCert, useQueue=useQueue, serverName=self._serverName, connection=connection)

class TestDOH3XFR(QUICXFRTests, DNSDistTest):
    _serverKey = 'server.key'
    _serverCert = 'server.chain'
    _serverName = 'tls.tests.dnsdist.org'
    _caCert = 'ca.pem'
    _doqServerPort = pickAvailablePort()
    _dohBaseURL = ("https://%s:%d/" % (_serverName, _doqServerPort))
    _config_template = """
    newServer{address="127.0.0.1:%d", tcpOnly=true}

    addDOH3Local("127.0.0.1:%d", "%s", "%s")
    """
    _config_params = ['_testServerPort', '_doqServerPort','_serverCert', '_serverKey']
    _verboseMode = True

    def getQUICConnection(self):
        return self.getDOQConnection(self._doqServerPort, self._caCert)

    def sendQUICQuery(self, query, response=None, useQueue=True, connection=None):
        return self.sendDOH3Query(self._doqServerPort, self._dohBaseURL, query, response=response, caFile=self._caCert, useQueue=useQueue, serverName=self._serverName, connection=connection)
