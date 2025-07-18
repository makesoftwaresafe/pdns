#ifndef BOOST_TEST_DYN_LINK
#define BOOST_TEST_DYN_LINK
#endif

#define BOOST_TEST_NO_MAIN

#include <boost/test/unit_test.hpp>

#include <cmath>
#include <numeric>
#include <unordered_set>

#include "dnsname.hh"
#include "misc.hh"
#include "dnswriter.hh"
#include "dnsrecords.hh"

using namespace boost;
using std::string;

BOOST_AUTO_TEST_SUITE(test_dnsname_cc)

BOOST_AUTO_TEST_CASE(test_basic) {
  DNSName aroot("a.root-servers.net"), broot("b.root-servers.net");
  BOOST_CHECK(aroot < broot);
  BOOST_CHECK(!(broot < aroot));
  BOOST_CHECK(aroot.canonCompare_three_way(broot) < 0);
  BOOST_CHECK(broot.canonCompare_three_way(aroot) > 0);


  string before("www.ds9a.nl.");
  DNSName b(before);
  BOOST_CHECK_EQUAL(b.getRawLabels().size(), 3U);
  string after(b.toString());
  BOOST_CHECK_EQUAL(before, after);

  DNSName jpmens("ccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc.bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.test.xxx.yyy-yyyy.zzzzzzzzz-test.");

  BOOST_CHECK_EQUAL(jpmens.toString(), "ccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc.bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.test.xxx.yyy-yyyy.zzzzzzzzz-test.");

  DNSName wwwds9anl("www.ds9a.nl.");
  DNSName wwwds9anl1("www.ds9a\002nl.");
  DNSName nl("nl.");
  BOOST_CHECK(wwwds9anl.isPartOf(nl));
  BOOST_CHECK(!wwwds9anl1.isPartOf(nl));
  BOOST_CHECK(wwwds9anl.isPartOf(wwwds9anl));

  BOOST_CHECK(!nl.isPartOf(wwwds9anl));

  BOOST_CHECK(wwwds9anl == wwwds9anl);

  BOOST_CHECK(DNSName("wWw.ds9A.Nl.") == DNSName("www.ds9a.nl."));
  BOOST_CHECK(DNSName("www.ds9a.nl.") == DNSName("www.ds9a.nl."));

  BOOST_CHECK(DNSName("www.ds9a.nl.").toString() == "www.ds9a.nl.");


  { // Check root vs empty
    DNSName name("."); // root
    DNSName parent; // empty
    BOOST_CHECK(name != parent);
  }

  { // Check name part of root
    DNSName name("a.");
    DNSName parent(".");
    BOOST_CHECK(name.isPartOf(parent));
  }

  { // Label boundary
    DNSName name("a\002bb.");
    DNSName parent("bb.");
    BOOST_CHECK(!name.isPartOf(parent));
  }

  { // Multi label parent
    DNSName name("a.bb.ccc.dddd.");
    DNSName parent("ccc.dddd.");
    BOOST_CHECK(name.isPartOf(parent));
  }

  { // Last char diff
    DNSName name("a.bb.ccc.dddd.");
    DNSName parent("ccc.dddx.");
    BOOST_CHECK(!name.isPartOf(parent));
  }

  { // Equal length identical
    DNSName name("aaaa.bbb.cc.d.");
    DNSName parent("aaaa.bbb.cc.d.");
    BOOST_CHECK(name.isPartOf(parent));
  }

  { // Equal length first char diff
    DNSName name("xaaa.bbb.cc.d.");
    DNSName parent("aaaa.bbb.cc.d.");
    BOOST_CHECK(!name.isPartOf(parent));
  }

  { // Make relative
    DNSName name("aaaa.bbb.cc.d.");
    DNSName parent("cc.d.");
    BOOST_CHECK_EQUAL( name.makeRelative(parent), DNSName("aaaa.bbb."));
  }

  { // Labelreverse
    DNSName name("aaaa.bbb.cc.d.");
    BOOST_CHECK( name.labelReverse() == DNSName("d.cc.bbb.aaaa."));
  }

  { // empty() empty
    DNSName name;
    BOOST_CHECK(name.empty());
  }

  { // empty() root
    DNSName name(".");
    BOOST_CHECK(!name.empty());

    DNSName rootnodot("");
    BOOST_CHECK_EQUAL(name, rootnodot);

    string empty;
    DNSName rootnodot2(empty);
    BOOST_CHECK_EQUAL(rootnodot2, name);
  }

  DNSName left("ds9a.nl.");
  left.prependRawLabel("www");
  BOOST_CHECK( left == DNSName("WwW.Ds9A.Nl."));

  left.appendRawLabel("com");

  BOOST_CHECK( left == DNSName("WwW.Ds9A.Nl.com."));

  DNSName unset;

  unset.appendRawLabel("www");
  unset.appendRawLabel("powerdns.com");
  unset.appendRawLabel("com");

  BOOST_CHECK_EQUAL(unset.toString(), "www.powerdns\\.com.com.");

  DNSName rfc4343_2_1("~!.example.");
  DNSName rfc4343_2_2(R"(Donald\032E\.\032Eastlake\0323rd.example.)");
  DNSName example("example.");
  BOOST_CHECK(rfc4343_2_1.isPartOf(example));
  BOOST_CHECK(rfc4343_2_2.isPartOf(example));
  BOOST_CHECK_EQUAL(rfc4343_2_1.toString(), "~!.example.");

  auto labels=rfc4343_2_2.getRawLabels();
  BOOST_CHECK_EQUAL(*labels.begin(), "Donald E. Eastlake 3rd");
  BOOST_CHECK_EQUAL(*labels.rbegin(), "example");
  BOOST_CHECK_EQUAL(labels.size(), 2U);

  DNSName build;
  build.appendRawLabel("Donald E. Eastlake 3rd");
  build.appendRawLabel("example");
  BOOST_CHECK_EQUAL(build.toString(), R"(Donald\032E\.\032Eastlake\0323rd.example.)");
  BOOST_CHECK_THROW(DNSName broken("bert..hubert."), std::runtime_error);

  DNSName n;
  n.appendRawLabel("powerdns.dnsmaster");
  n.appendRawLabel("powerdns");
  n.appendRawLabel("com");

  BOOST_CHECK_EQUAL(n.toString(), "powerdns\\.dnsmaster.powerdns.com.");

  //  BOOST_CHECK(DNSName().toString() != ".");

  DNSName p;
  string label("power");
  label.append(1, (char)0);
  label.append("dns");
  p.appendRawLabel(label);
  p.appendRawLabel("com");

  BOOST_CHECK_EQUAL(p.toString(), "power\\000dns.com.");
}

BOOST_AUTO_TEST_CASE(test_trim) {
  DNSName w("www.powerdns.com.");
  BOOST_CHECK_EQUAL(w.countLabels(), 3U);
  w.trimToLabels(2);
  BOOST_CHECK_EQUAL(w.toString(), "powerdns.com.");
  DNSName w2("powerdns.com.");
  BOOST_CHECK(w==w2);

  DNSName root(".");
  BOOST_CHECK_EQUAL(root.countLabels(), 0U);
}

BOOST_AUTO_TEST_CASE(test_toolong) {

  BOOST_CHECK_THROW(DNSName w("1234567890123456789012345678901234567890123456789012345678901234567890.com."), std::range_error);

  BOOST_CHECK_THROW(DNSName w("12345678901234567890.12345678901234567890123456.789012345678901.234567890.12345678901234567890.12345678901234567890123456.789012345678901.234567890.12345678901234567890.12345678901234567890123456.789012345678901.234567890.234567890.789012345678901.234567890.234567890.789012345678901.234567890.234567890.com."), std::range_error);
}

BOOST_AUTO_TEST_CASE(test_dnsstrings) {
  DNSName w("www.powerdns.com.");
  BOOST_CHECK_EQUAL(w.toDNSString(), string("\003www\010powerdns\003com\000", 18));
}

BOOST_AUTO_TEST_CASE(test_empty) {
  DNSName empty;
  BOOST_CHECK_THROW(empty.toString(), std::out_of_range);
  BOOST_CHECK_THROW(empty.toStringNoDot(), std::out_of_range);
  BOOST_CHECK_THROW(empty.toDNSString(), std::out_of_range);
  BOOST_CHECK(empty.empty());
  BOOST_CHECK(!empty.isRoot());
  BOOST_CHECK(!empty.isWildcard());
  BOOST_CHECK_EQUAL(empty, empty);
  BOOST_CHECK(!(empty < empty));

  DNSName root(".");
  BOOST_CHECK(empty < root);

  BOOST_CHECK_THROW(empty.isPartOf(root), std::out_of_range);
  BOOST_CHECK_THROW(root.isPartOf(empty), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(test_specials) {
  DNSName root(".");

  BOOST_CHECK(root.isRoot());
  BOOST_CHECK(root != DNSName());

  DNSName wcard("*.powerdns.com");
  BOOST_CHECK(wcard.isWildcard());

  DNSName notwcard("www.powerdns.com");
  BOOST_CHECK(!notwcard.isWildcard());
}


BOOST_AUTO_TEST_CASE(test_chopping) {
  DNSName w("www.powerdns.com.");
  BOOST_CHECK_EQUAL(w.toString(), "www.powerdns.com.");
  BOOST_CHECK(w.chopOff());
  BOOST_CHECK_EQUAL(w.toString(), "powerdns.com.");
  BOOST_CHECK(w.chopOff());
  BOOST_CHECK_EQUAL(w.toString(), "com.");
  BOOST_CHECK(w.chopOff());
  BOOST_CHECK_EQUAL(w.toString(), ".");
  BOOST_CHECK(!w.chopOff());
  BOOST_CHECK(!w.chopOff());

  w.prependRawLabel("net");
  w.prependRawLabel("root-servers");
  w.prependRawLabel("a");
  BOOST_CHECK_EQUAL(w.toString(), "a.root-servers.net.");
}

BOOST_AUTO_TEST_CASE(test_Append) {
  DNSName dn("www."), powerdns("powerdns.com.");
  DNSName tot=dn+powerdns;

  BOOST_CHECK_EQUAL(tot.toString(), "www.powerdns.com.");
  BOOST_CHECK(tot == DNSName("www.powerdns.com."));

  dn+=powerdns;

  BOOST_CHECK(dn == DNSName("www.powerdns.com."));
}

BOOST_AUTO_TEST_CASE(test_packetCompress) {
  vector<unsigned char> packet;
  DNSPacketWriter dpw(packet, DNSName("www.ds9a.nl."), QType::AAAA);
  dpw.startRecord(DNSName("ds9a.nl"), QType::SOA);
  SOARecordContent src("ns1.powerdns.nl admin.powerdns.nl 1 2 3 4 5");
  src.toPacket(dpw);
  AAAARecordContent aaaa("::1");
  dpw.startRecord(DNSName("www.dS9A.nl"), QType::AAAA);
  aaaa.toPacket(dpw);
  dpw.startRecord(DNSName("www.ds9A.nl"), QType::AAAA);
  aaaa.toPacket(dpw);
  dpw.startRecord(DNSName("www.dS9a.nl"), QType::AAAA);
  aaaa.toPacket(dpw);
  dpw.startRecord(DNSName("www2.DS9a.nl"), QType::AAAA);
  aaaa.toPacket(dpw);
  dpw.startRecord(DNSName("www2.dS9a.nl"), QType::AAAA);
  aaaa.toPacket(dpw);
  dpw.commit();
  string str((const char*)&packet[0], (const char*)&packet[0] + packet.size());
  size_t pos = 0;
  int count=0;
  while((pos = str.find("ds9a", pos)) != string::npos) {
    ++pos;
    ++count;
  }
  BOOST_CHECK_EQUAL(count, 1);
  pos = 0;
  count=0;
  while((pos = str.find("powerdns", pos)) != string::npos) {
    ++pos;
    ++count;
  }
  BOOST_CHECK_EQUAL(count, 1);

}

BOOST_AUTO_TEST_CASE(test_packetCompressLong) {
  vector<unsigned char> packet;
  DNSName loopback("1.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.ip6.arpa");
  DNSPacketWriter dpw(packet, loopback, QType::PTR);

  dpw.startRecord(loopback, QType::PTR);
  PTRRecordContent prc(DNSName("localhost"));
  prc.toPacket(dpw);
  dpw.commit();
  DNSName roundtrip((char*)&packet[0], packet.size(), 12, false);
  BOOST_CHECK_EQUAL(loopback,roundtrip);

  packet.clear();
  DNSName longer("1.2.3.4.5.6.7.8.1.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.ip6.arpa");
  DNSPacketWriter dpw2(packet, longer, QType::PTR);

  dpw2.startRecord(DNSName("a.b.c.d.e")+longer, QType::PTR);
  PTRRecordContent prc2(DNSName("localhost"));
  prc2.toPacket(dpw2);
  dpw2.commit();

}




BOOST_AUTO_TEST_CASE(test_PacketParse) {
  vector<unsigned char> packet;
  DNSName root(".");
  DNSPacketWriter dpw1(packet, g_rootdnsname, QType::AAAA);
  DNSName p((char*)&packet[0], packet.size(), 12, false);
  BOOST_CHECK_EQUAL(p, root);
  unsigned char* buffer=&packet[0];
  /* set invalid label len:
     - packet.size() == 17 (sizeof(dnsheader) + 1 + 2 + 2)
     - label len < packet.size() but
     - offset is 12, label len of 15 should be rejected
     because offset + 15 >= packet.size()
  */
  buffer[sizeof(dnsheader)] = 15;
  BOOST_CHECK_THROW(DNSName((char*)&packet[0], packet.size(), 12, false), std::range_error);
}


BOOST_AUTO_TEST_CASE(test_hash) {
  DNSName a("wwW.Ds9A.Nl"), b("www.ds9a.nl");
  BOOST_CHECK_EQUAL(a.hash(), b.hash());

  vector<uint32_t> counts(1500);

  for(unsigned int n=0; n < 100000; ++n) {
    DNSName dn(std::to_string(n)+"."+std::to_string(n*2)+"ds9a.nl");
    DNSName dn2(std::to_string(n)+"."+std::to_string(n*2)+"Ds9a.nL");
    BOOST_CHECK_EQUAL(dn.hash(), dn2.hash());
    counts[dn.hash() % counts.size()]++;
  }

  double sum = std::accumulate(std::begin(counts), std::end(counts), 0.0);
  double m =  sum / counts.size();

  double accum = 0.0;
  std::for_each (std::begin(counts), std::end(counts), [&](const double d) {
      accum += (d - m) * (d - m);
  });

  double stdev = sqrt(accum / (counts.size()-1));
  BOOST_CHECK(stdev < 10);
}

BOOST_AUTO_TEST_CASE(test_hashContainer) {
  std::unordered_set<DNSName> s;
  s.insert(DNSName("www.powerdns.com"));
  BOOST_CHECK(s.count(DNSName("WwW.PoWerDNS.CoM")));
  BOOST_CHECK_EQUAL(s.size(), 1U);
  s.insert(DNSName("www.POWERDNS.com"));
  BOOST_CHECK_EQUAL(s.size(), 1U);
  s.insert(DNSName("www2.POWERDNS.com"));
  BOOST_CHECK_EQUAL(s.size(), 2U);

  s.clear();
  unsigned int n=0;
  for(; n < 100000; ++n)
    s.insert(DNSName(std::to_string(n)+".test.nl"));
  BOOST_CHECK_EQUAL(s.size(), n);

}


BOOST_AUTO_TEST_CASE(test_QuestionHash) {
  vector<unsigned char> packet(sizeof(dnsheader));

  bool ok;
  // A return init case
  BOOST_CHECK_EQUAL(hashQuestion(packet.data(), sizeof(dnsheader), 0xffU, ok), 0xffU);
  BOOST_CHECK(!ok);

  // We subtract 4 from the packet sizes since DNSPacketWriter adds a type and a class
  // We expect the hash of the root to be unequal to the burtle init value
  DNSPacketWriter dpw0(packet, DNSName("."), QType::AAAA);
  BOOST_CHECK(hashQuestion(packet.data(), packet.size() - 4, 0xffU, ok) != 0xffU);
  BOOST_CHECK(ok);

  // A truncated buffer should return the init value
  DNSPacketWriter dpw1(packet, DNSName("."), QType::AAAA);
  BOOST_CHECK_EQUAL(hashQuestion(packet.data(), packet.size() - 5, 0xffU, ok), 0xffU);
  BOOST_CHECK(!ok);

  DNSPacketWriter dpw2(packet, DNSName("www.ds9a.nl."), QType::AAAA);
  // Let's make an invalid name by overwriting the length of the second label just outside the buffer
  packet[sizeof(dnsheader) + 4] = 8;
  BOOST_CHECK_EQUAL(hashQuestion(packet.data(), packet.size() - 4, 0xffU, ok), 0xffU);
  BOOST_CHECK(!ok);

  DNSPacketWriter dpw3(packet, DNSName("www.ds9a.nl."), QType::AAAA);
  // Let's make an invalid name by overwriting the length of the second label way outside the buffer
  packet[sizeof(dnsheader) + 4] = 0xff;
  BOOST_CHECK_EQUAL(hashQuestion(packet.data(), packet.size() - 4, 0xffU, ok), 0xffU);
  BOOST_CHECK(!ok);

  DNSPacketWriter dpw4(packet, DNSName("www.ds9a.nl."), QType::AAAA);
  auto hash1 = hashQuestion(&packet[0], packet.size() - 4, 0, ok);
  BOOST_CHECK(ok);
  DNSPacketWriter dpw5(packet, DNSName("wWw.Ds9A.nL."), QType::AAAA);
  auto hash2 = hashQuestion(&packet[0], packet.size() - 4, 0, ok);
  BOOST_CHECK_EQUAL(hash1, hash2);
  BOOST_CHECK(ok);

  vector<uint32_t> counts(1500);
  for(unsigned int n = 0; n < 100000; ++n) {
    packet.clear();
    DNSPacketWriter dpw(packet, DNSName(std::to_string(n) + "." + std::to_string(n*2) + "."), QType::AAAA);
    assert(ok);
    counts[hashQuestion(&packet[0], packet.size() - 4, 0, ok) % counts.size()]++;
  }

  double sum = std::accumulate(std::begin(counts), std::end(counts), 0.0);
  double m =  sum / counts.size();

  double accum = 0.0;
  std::for_each (std::begin(counts), std::end(counts), [&](const double d) {
      accum += (d - m) * (d - m);
  });

  double stdev = sqrt(accum / (counts.size()-1));
  BOOST_CHECK(stdev < 10);
}

BOOST_AUTO_TEST_CASE(test_packetParse) {
  vector<unsigned char> packet;
  DNSPacketWriter dpw(packet, DNSName("www.ds9a.nl."), QType::AAAA);

  uint16_t qtype, qclass;
  DNSName dn((char*)&packet[0], packet.size(), 12, false, &qtype, &qclass);
  BOOST_CHECK_EQUAL(dn.toString(), "www.ds9a.nl.");
  BOOST_CHECK(qtype == QType::AAAA);
  BOOST_CHECK_EQUAL(qclass, 1);

  dpw.startRecord(DNSName("ds9a.nl."), DNSRecordContent::TypeToNumber("NS"));
  NSRecordContent nrc("ns1.powerdns.com");
  nrc.toPacket(dpw);

  dpw.commit();

  /* packet now looks like this:
     012345678901 12 bytes of header
     3www4ds9a2nl0 13 bytes of name
     0001 0001      4 bytes of qtype and qclass
     answername     2 bytes
     0001 0001      4 bytes of qtype and class
     0000 0000      4 bytes of TTL
     0000           2 bytes of content length
     content name */

  DNSName dn2((char*)&packet[0], packet.size(), 12+13+4, true, &qtype, &qclass);
  BOOST_CHECK_EQUAL(dn2.toString(), "ds9a.nl.");
  BOOST_CHECK(qtype == QType::NS);
  BOOST_CHECK_EQUAL(qclass, 1);

  DNSName dn3((char*)&packet[0], packet.size(), 12+13+4+2 + 4 + 4 + 2, true);
  BOOST_CHECK_EQUAL(dn3.toString(), "ns1.powerdns.com.");
  try {
    DNSName dn4((char*)&packet[0], packet.size(), 12+13+4, false); // compressed, should fail
    BOOST_CHECK(0);
  }
  catch(...){}
}

BOOST_AUTO_TEST_CASE(test_escaping) {
  DNSName n;
  string label;

  for(int i = 0; i < 250; ++i) {
    if(!((i+1)%63)) {
      n.appendRawLabel(label);
      label.clear();
    }
    label.append(1,(char)i);
  }
  if(!label.empty())
    n.appendRawLabel(label);

  DNSName n2(n.toString());
  BOOST_CHECK(n==n2);
}

BOOST_AUTO_TEST_CASE(test_suffixmatch) {
  SuffixMatchNode smn;
  DNSName ezdns("ezdns.it.");
  smn.add(ezdns.getRawLabels());

  smn.add(DNSName("org.").getRawLabels());

  DNSName wwwpowerdnscom("www.powerdns.com.");
  DNSName wwwezdnsit("www.ezdns.it.");
  BOOST_CHECK(smn.check(wwwezdnsit));
  BOOST_CHECK(!smn.check(wwwpowerdnscom));

  BOOST_CHECK(smn.check(DNSName("www.powerdns.org.")));
  BOOST_CHECK(smn.check(DNSName("www.powerdns.oRG.")));

  smn.add(DNSName("news.bbc.co.uk."));
  BOOST_CHECK(smn.check(DNSName("news.bbc.co.uk.")));
  BOOST_CHECK(smn.getBestMatch(DNSName("news.bbc.co.uk")) == DNSName("news.bbc.co.uk."));
  BOOST_CHECK(smn.check(DNSName("www.news.bbc.co.uk.")));
  BOOST_CHECK(smn.getBestMatch(DNSName("www.news.bbc.co.uk")) == DNSName("news.bbc.co.uk."));
  BOOST_CHECK(smn.check(DNSName("www.www.www.www.www.news.bbc.co.uk.")));
  BOOST_CHECK(!smn.check(DNSName("images.bbc.co.uk.")));
  BOOST_CHECK(smn.getBestMatch(DNSName("images.bbc.co.uk")) == std::nullopt);

  BOOST_CHECK(!smn.check(DNSName("www.news.gov.uk.")));
  BOOST_CHECK(smn.getBestMatch(DNSName("www.news.gov.uk")) == std::nullopt);

  smn.add(g_rootdnsname); // block the root
  BOOST_CHECK(smn.check(DNSName("a.root-servers.net.")));
  BOOST_CHECK(smn.getBestMatch(DNSName("a.root-servers.net.")) == g_rootdnsname);

  DNSName examplenet("example.net.");
  DNSName net("net.");
  smn.add(examplenet);
  smn.add(net);
  BOOST_CHECK(smn.check(examplenet));
  BOOST_CHECK(smn.check(net));

  // Remove .net and the root, and check that example.net still exists
  smn.remove(g_rootdnsname);
  smn.remove(net);
  BOOST_CHECK_EQUAL(smn.check(net), false);
  BOOST_CHECK(smn.check(examplenet));

  smn.add(DNSName("fr."));
  smn.add(DNSName("www.sub.domain.fr."));
  // should not match www.sub.domain.fr. but should still match fr.
  BOOST_CHECK(smn.check(DNSName("sub.domain.fr.")));
}

BOOST_AUTO_TEST_CASE(test_suffixmatch_tree) {
  SuffixMatchTree<DNSName> smt;
  DNSName ezdns("ezdns.it.");
  smt.add(ezdns, DNSName(ezdns));

  auto labels = DNSName("org.").getRawLabels();
  smt.add(labels, DNSName("org."));

  DNSName wwwpowerdnscom("www.powerdns.com.");
  DNSName wwwezdnsit("www.ezdns.it.");
  BOOST_REQUIRE(smt.lookup(wwwezdnsit));
  BOOST_CHECK_EQUAL(*smt.lookup(wwwezdnsit), ezdns);
  BOOST_CHECK(smt.lookup(wwwpowerdnscom) == nullptr);

  BOOST_REQUIRE(smt.lookup(DNSName("www.powerdns.org.")));
  BOOST_CHECK_EQUAL(*smt.lookup(DNSName("www.powerdns.org.")), DNSName("org."));
  BOOST_REQUIRE(smt.lookup(DNSName("www.powerdns.oRG.")));
  BOOST_CHECK_EQUAL(*smt.lookup(DNSName("www.powerdns.oRG.")), DNSName("org."));

  smt.add(DNSName("news.bbc.co.uk."), DNSName("news.bbc.co.uk."));
  BOOST_REQUIRE(smt.lookup(DNSName("news.bbc.co.uk.")));
  BOOST_CHECK_EQUAL(*smt.lookup(DNSName("news.bbc.co.uk.")), DNSName("news.bbc.co.uk."));
  BOOST_REQUIRE(smt.lookup(DNSName("www.news.bbc.co.uk.")));
  BOOST_CHECK_EQUAL(*smt.lookup(DNSName("www.news.bbc.co.uk.")), DNSName("news.bbc.co.uk."));
  BOOST_REQUIRE(smt.lookup(DNSName("www.www.www.www.www.news.bbc.co.uk.")));
  BOOST_CHECK_EQUAL(*smt.lookup(DNSName("www.www.www.www.www.news.bbc.co.uk.")), DNSName("news.bbc.co.uk."));
  BOOST_CHECK(smt.lookup(DNSName("images.bbc.co.uk.")) == nullptr);
  BOOST_CHECK(smt.lookup(DNSName("www.news.gov.uk.")) == nullptr);

  smt.add(g_rootdnsname, DNSName(g_rootdnsname)); // block the root
  BOOST_REQUIRE(smt.lookup(DNSName("a.root-servers.net.")));
  BOOST_CHECK_EQUAL(*smt.lookup(DNSName("a.root-servers.net.")), g_rootdnsname);

  DNSName apowerdnscom("a.powerdns.com.");
  DNSName bpowerdnscom("b.powerdns.com.");
  smt.add(apowerdnscom, DNSName(apowerdnscom));
  smt.add(bpowerdnscom, DNSName(bpowerdnscom));
  BOOST_REQUIRE(smt.lookup(apowerdnscom));
  BOOST_CHECK_EQUAL(*smt.lookup(apowerdnscom), apowerdnscom);
  BOOST_REQUIRE(smt.lookup(bpowerdnscom));
  BOOST_CHECK_EQUAL(*smt.lookup(bpowerdnscom), bpowerdnscom);

  DNSName examplenet("example.net.");
  DNSName net("net.");
  smt.add(examplenet, DNSName(examplenet));
  smt.add(net, DNSName(net));
  BOOST_REQUIRE(smt.lookup(examplenet));
  BOOST_CHECK_EQUAL(*smt.lookup(examplenet), examplenet);
  BOOST_REQUIRE(smt.lookup(net));
  BOOST_CHECK_EQUAL(*smt.lookup(net), net);

  // Remove .net and the root, and check that example.net remains
  smt.remove(g_rootdnsname);
  smt.remove(net);
  BOOST_CHECK(smt.lookup(net) == nullptr);
  BOOST_CHECK_EQUAL(*smt.lookup(examplenet), examplenet);

  smt = SuffixMatchTree<DNSName>();
  smt.add(examplenet, DNSName(examplenet));
  smt.add(net, DNSName(net));
  smt.add(DNSName("news.bbc.co.uk."), DNSName("news.bbc.co.uk."));
  smt.add(apowerdnscom, DNSName(apowerdnscom));

  smt.remove(DNSName("not-such-entry.news.bbc.co.uk."));
  BOOST_REQUIRE(smt.lookup(DNSName("news.bbc.co.uk.")));
  smt.remove(DNSName("news.bbc.co.uk."));
  BOOST_CHECK(smt.lookup(DNSName("news.bbc.co.uk.")) == nullptr);

  smt.remove(net);
  BOOST_REQUIRE(smt.lookup(examplenet));
  BOOST_CHECK_EQUAL(*smt.lookup(examplenet), examplenet);
  BOOST_CHECK(smt.lookup(net) == nullptr);

  smt.remove(examplenet);
  BOOST_CHECK(smt.lookup(net) == nullptr);
  BOOST_CHECK(smt.lookup(examplenet) == nullptr);

  smt.add(examplenet, DNSName(examplenet));
  smt.add(net, DNSName(net));
  BOOST_REQUIRE(smt.lookup(examplenet));
  BOOST_CHECK_EQUAL(*smt.lookup(examplenet), examplenet);
  BOOST_REQUIRE(smt.lookup(net));
  BOOST_CHECK_EQUAL(*smt.lookup(net), net);

  smt.remove(examplenet);
  BOOST_CHECK_EQUAL(*smt.lookup(examplenet), net);
  BOOST_CHECK_EQUAL(*smt.lookup(net), net);
  smt.remove(examplenet);
  BOOST_CHECK_EQUAL(*smt.lookup(examplenet), net);
  BOOST_CHECK_EQUAL(*smt.lookup(net), net);
  smt.remove(net);
  BOOST_CHECK(smt.lookup(net) == nullptr);
  BOOST_CHECK(smt.lookup(examplenet) == nullptr);
  smt.remove(net);

  size_t count = 0;
  smt.visit([apowerdnscom, &count](const SuffixMatchTree<DNSName>& smtarg) {
      count++;
      BOOST_CHECK_EQUAL(smtarg.d_value, apowerdnscom);
    });
  BOOST_CHECK_EQUAL(count, 1U);

  BOOST_CHECK_EQUAL(*smt.lookup(apowerdnscom), apowerdnscom);
  smt.remove(apowerdnscom);
  BOOST_CHECK(smt.lookup(apowerdnscom) == nullptr);

  count = 0;
  smt.visit([&count](const SuffixMatchTree<DNSName>&) {
      count++;
    });
  BOOST_CHECK_EQUAL(count, 0U);
}


BOOST_AUTO_TEST_CASE(test_concat) {
  DNSName first("www."), second("powerdns.com.");
  BOOST_CHECK_EQUAL((first+second).toString(), "www.powerdns.com.");
}

BOOST_AUTO_TEST_CASE(test_compare_naive) {
  BOOST_CHECK(DNSName("abc.com.") < DNSName("zdf.com."));
  BOOST_CHECK(DNSName("Abc.com.") < DNSName("zdf.com."));
  BOOST_CHECK(DNSName("Abc.com.") < DNSName("Zdf.com."));
  BOOST_CHECK(DNSName("abc.com.") < DNSName("Zdf.com."));
}

BOOST_AUTO_TEST_CASE(test_compare_empty) {
  DNSName a, b;
  BOOST_CHECK(!(a<b));
  BOOST_CHECK(a.canonCompare_three_way(b) == 0);
}

BOOST_AUTO_TEST_CASE(test_casing) {
  DNSName a("WwW.PoWeRdNS.Com"), b("www.powerdns.com.");
  BOOST_CHECK_EQUAL(a,b);
  BOOST_CHECK_EQUAL(a.toString(), "WwW.PoWeRdNS.Com.");
  DNSName c=a.makeLowerCase();
  BOOST_CHECK_EQUAL(a,c);
  BOOST_CHECK_EQUAL(b,c);
  BOOST_CHECK_EQUAL(c.toString(), b.toString());
  BOOST_CHECK_EQUAL(c.toString(), "www.powerdns.com.");
}



BOOST_AUTO_TEST_CASE(test_compare_canonical) {
  DNSName lower("bert.com."), higher("alpha.nl.");
  BOOST_CHECK(lower.canonCompare_three_way(higher) < 0);

  BOOST_CHECK(DNSName("bert.com").canonCompare_three_way(DNSName("www.bert.com")) < 0);
  BOOST_CHECK(DNSName("BeRt.com").canonCompare_three_way(DNSName("WWW.berT.com")) < 0);
  BOOST_CHECK(DNSName("www.BeRt.com").canonCompare_three_way(DNSName("WWW.berT.com")) == 0);

  CanonDNSNameCompare a;
  BOOST_CHECK(a(g_rootdnsname, DNSName("www.powerdns.com")));
  BOOST_CHECK(a(g_rootdnsname, DNSName("www.powerdns.net")));
  BOOST_CHECK(!a(DNSName("www.powerdns.net"), g_rootdnsname));

  vector<DNSName> vec;
  for(const char* b : {"bert.com.", "alpha.nl.", "articles.xxx.",
	"Aleph1.powerdns.com.", "ZOMG.powerdns.com.", "aaa.XXX.", "yyy.XXX.",
	"test.powerdns.com.", "\\128.com"}) {
    vec.push_back(DNSName(b));
  }
  sort(vec.begin(), vec.end(), CanonDNSNameCompare());
  //  for(const auto& v : vec)
  //    cerr<<'"'<<v<<'"'<<endl;

  vector<DNSName> right;
  for(const auto& b: {"bert.com.",  "Aleph1.powerdns.com.",
	"test.powerdns.com.",
	"ZOMG.powerdns.com.",
	"\\128.com.",
	"alpha.nl.",
	"aaa.XXX.",
	"articles.xxx.",
	"yyy.XXX."})
    right.push_back(DNSName(b));


  BOOST_CHECK(vec==right);
}


BOOST_AUTO_TEST_CASE(test_empty_label) { // empty label

  { // append
    DNSName dn("www.");
    BOOST_CHECK_THROW(dn.appendRawLabel(""), std::range_error);
  }

  { // prepend
    DNSName dn("www.");
    BOOST_CHECK_THROW(dn.prependRawLabel(""), std::range_error);
  }
}

BOOST_AUTO_TEST_CASE(test_label_length_max) { // 63 char label

  string label("123456789012345678901234567890123456789012345678901234567890123");

  { // append
    DNSName dn("www.");
    dn.appendRawLabel(label);
    BOOST_CHECK_EQUAL(dn.toString(), "www." + label + ".");
  }

  { // prepend
    DNSName dn("www.");
    dn.prependRawLabel(label);
    BOOST_CHECK_EQUAL(dn.toString(), label + ".www.");
  }
}

BOOST_AUTO_TEST_CASE(test_label_length_too_long) { // 64 char label

  string label("1234567890123456789012345678901234567890123456789012345678901234");

  { // append
    DNSName dn("www.");
    BOOST_CHECK_THROW(dn.appendRawLabel(label), std::range_error);
  }

  { // prepend
    DNSName dn("www.");
    BOOST_CHECK_THROW(dn.prependRawLabel(label), std::range_error);
  }
}

BOOST_AUTO_TEST_CASE(test_name_length_max) { // 255 char name

  string name("123456789.123456789.123456789.123456789.123456789.123456789.123456789.123456789.123456789.123456789."
              "123456789.123456789.123456789.123456789.123456789.123456789.123456789.123456789.123456789.123456789."
              "123456789.123456789.123456789.123456789.123456789.");
  string label("123");

  { // append
    DNSName dn(name);
    dn.appendRawLabel(label);
    BOOST_CHECK_EQUAL(dn.toString().size(), 254U);
  }

  { // prepend
    DNSName dn(name);
    dn.prependRawLabel(label);
    BOOST_CHECK_EQUAL(dn.toString().size(), 254U);
  }

  { // concat
    DNSName dn(name);

    dn += DNSName(label + ".");
    BOOST_CHECK_EQUAL(dn.toString().size(), 254U);
  }
}

BOOST_AUTO_TEST_CASE(test_name_length_too_long) { // 256 char name

  string name("123456789.123456789.123456789.123456789.123456789.123456789.123456789.123456789.123456789.123456789."
              "123456789.123456789.123456789.123456789.123456789.123456789.123456789.123456789.123456789.123456789."
              "123456789.123456789.123456789.123456789.123456789.");
  string label("1234");

  { // append
    DNSName dn(name);
    BOOST_CHECK_THROW(dn.appendRawLabel(label), std::range_error);
  }

  { // prepend
    DNSName dn(name);
    BOOST_CHECK_THROW(dn.prependRawLabel(label), std::range_error);
  }

  { // concat
    DNSName dn(name);
    BOOST_CHECK_THROW(dn += DNSName(label + "."), std::range_error);
  }
}


BOOST_AUTO_TEST_CASE(test_invalid_label_length) { // Invalid label length in qname

  string name("\x02""ns\x07""example\x04""com\x00", 16);

  BOOST_CHECK_THROW(DNSName dn(name.c_str(), name.size(), 0, true), std::range_error);
}

BOOST_AUTO_TEST_CASE(test_compression) { // Compression test

  string name("\x03""com\x00""\x07""example\xc0""\x00""\x03""www\xc0""\x05", 21);

  DNSName dn(name.c_str(), name.size(), 15, true);
  BOOST_CHECK_EQUAL(dn.toString(), "www.example.com.");
}

BOOST_AUTO_TEST_CASE(test_compression_qtype_qclass) { // Compression test with QClass and QType extraction

  uint16_t qtype = 0;
  uint16_t qclass = 0;

  {
    string name("\x03""com\x00""\x07""example\xc0""\x00""\x03""www\xc0""\x05""\x00""\x01""\x00""\x01", 25);
    DNSName dn(name.c_str(), name.size(), 15, true, &qtype, &qclass);
    BOOST_CHECK_EQUAL(dn.toString(), "www.example.com.");
    BOOST_CHECK_EQUAL(qtype, 1);
    BOOST_CHECK_EQUAL(qclass, 1);
  }

  {
    /* same but this time we are one byte short for the qclass */
    string name("\x03""com\x00""\x07""example\xc0""\x00""\x03""www\xc0""\x05""\x00""\x01""\x00""", 24);
    BOOST_CHECK_THROW(DNSName dn(name.c_str(), name.size(), 15, true, &qtype, &qclass), std::range_error);
  }

  {
    /* this time with a compression pointer such as (labellen << 8) != 0, see #4718 */
    string name("\x03""com\x00""\x07""example\xc1""\x00""\x03""www\xc1""\x05""\x00""\x01""\x00""\x01", 25);
    name.insert(0, 256, '0');

    DNSName dn(name.c_str(), name.size(), 271, true, &qtype, &qclass);
    BOOST_CHECK_EQUAL(dn.toString(), "www.example.com.");
    BOOST_CHECK_EQUAL(qtype, 1);
    BOOST_CHECK_EQUAL(qclass, 1);
  }

  {
    /* same but this time we are one byte short for the qclass */
    string name("\x03""com\x00""\x07""example\xc1""\x00""\x03""www\xc1""\x05""\x00""\x01""\x00", 24);
    name.insert(0, 256, '0');

    BOOST_CHECK_THROW(DNSName dn(name.c_str(), name.size(), 271, true, &qtype, &qclass), std::range_error);
  }
}

BOOST_AUTO_TEST_CASE(test_compression_single_bit_set) { // first 2 bits as 10 or 01, not 11

  // first 2 bits: 10
  {
    string name("\x03""com\x00""\x07""example\x80""\x00""\x03""www\x80""\x05", 21);

    BOOST_CHECK_THROW(DNSName dn(name.c_str(), name.size(), 15, true), std::range_error);
  }

  // first 2 bits: 01
  {
    string name("\x03""com\x00""\x07""example\x40""\x00""\x03""www\x40""\x05", 21);

    BOOST_CHECK_THROW(DNSName dn(name.c_str(), name.size(), 15, true), std::range_error);
  }

}

BOOST_AUTO_TEST_CASE(test_pointer_pointer_root) { // Pointer to pointer to root

  string name("\x00""\xc0""\x00""\x03""com\xc0""\x01",9);

  DNSName dn(name.c_str(), name.size(), 3, true);
  BOOST_CHECK_EQUAL(dn.toString(), "com.");
}

BOOST_AUTO_TEST_CASE(test_bad_compression_pointer) { // Pointing beyond packet boundary

  std::string name("\x03""com\x00""\x07""example\xc0""\x11""xc0""\x00", 17);

  BOOST_CHECK_THROW(DNSName dn(name.c_str(), name.length(), 5, true), std::range_error);
}

BOOST_AUTO_TEST_CASE(test_compression_loop) { // Compression loop (add one label)

  std::string name("\x03""www\xc0""\x00", 6);

  BOOST_CHECK_THROW(DNSName dn(name.c_str(), name.length(), 0, true), std::range_error);
}

BOOST_AUTO_TEST_CASE(test_compression_loop1) { // Compression loop (pointer loop)

  string name("\xc0""\x00", 2);

  BOOST_CHECK_THROW(DNSName dn(name.c_str(), name.size(), 0, true), std::range_error);
}

BOOST_AUTO_TEST_CASE(test_compression_loop2) { // Compression loop (deep recursion)

  int i;
  string name("\x00\xc0\x00", 3);
  for (i=0; i<98; ++i) {
    name.append( 1, ((i >> 7) & 0xff) | 0xc0);
    name.append( 1, ((i << 1) & 0xff) | 0x01);
  }
  BOOST_CHECK_NO_THROW(DNSName dn(name.c_str(), name.size(), name.size()-2, true));

  ++i;
  name.append( 1, ((i >> 7) & 0xff) | 0xc0);
  name.append( 1, ((i << 1) & 0xff) | 0x01);

  BOOST_CHECK_THROW(DNSName dn(name.c_str(), name.size(), name.size()-2, true), std::range_error);
}

BOOST_AUTO_TEST_CASE(test_wirelength) { // Testing if we get the correct value from the wirelength function
  DNSName name("www.powerdns.com");
  BOOST_CHECK_EQUAL(name.wirelength(), 18U);

  DNSName sname("powerdns.com");
  sname.prependRawLabel(string("ww\x00""w", 4));
  BOOST_CHECK_EQUAL(sname.wirelength(), 19U);

  sname = DNSName("powerdns.com");
  sname.prependRawLabel(string("www\x00", 4));
  BOOST_CHECK_EQUAL(sname.wirelength(), 19U);
}

BOOST_AUTO_TEST_CASE(test_getrawlabel) {
  DNSName name("a.bb.ccc.dddd.");
  BOOST_CHECK_EQUAL(name.getRawLabel(0), "a");
  BOOST_CHECK_EQUAL(name.getRawLabel(1), "bb");
  BOOST_CHECK_EQUAL(name.getRawLabel(2), "ccc");
  BOOST_CHECK_EQUAL(name.getRawLabel(3), "dddd");
  BOOST_CHECK_THROW(name.getRawLabel(name.countLabels()), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(test_getrawlabels_visitor) {
  DNSName name("a.bb.ccc.dddd.");
  auto visitor = name.getRawLabelsVisitor();
  BOOST_CHECK(!visitor.empty());
  BOOST_CHECK_EQUAL(visitor.front(), *name.getRawLabels().begin());
  BOOST_CHECK_EQUAL(visitor.back(), *name.getRawLabels().rbegin());

  BOOST_CHECK_EQUAL(visitor.back(), "dddd");
  BOOST_CHECK(visitor.pop_back());
  BOOST_CHECK_EQUAL(visitor.back(), "ccc");
  BOOST_CHECK(visitor.pop_back());
  BOOST_CHECK_EQUAL(visitor.back(), "bb");
  BOOST_CHECK(visitor.pop_back());
  BOOST_CHECK_EQUAL(visitor.back(), "a");
  BOOST_CHECK(visitor.pop_back());
  BOOST_CHECK(visitor.empty());
  BOOST_CHECK(!visitor.pop_back());
  BOOST_CHECK_THROW(visitor.front(), std::out_of_range);
  BOOST_CHECK_THROW(visitor.back(), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(test_getlastlabel) {
  DNSName name("www.powerdns.com");
  DNSName ans = name.getLastLabel();

  // Check the const-ness
  BOOST_CHECK_EQUAL(name, DNSName("www.powerdns.com"));

  // Check if the last label is indeed returned
  BOOST_CHECK_EQUAL(ans, DNSName("com"));
}

BOOST_AUTO_TEST_CASE(test_getcommonlabels) {
  const DNSName name1("www.powerdns.com");
  const DNSName name2("a.long.list.of.labels.powerdns.com");

  BOOST_CHECK_EQUAL(name1.getCommonLabels(name1), name1);
  BOOST_CHECK_EQUAL(name2.getCommonLabels(name2), name2);

  BOOST_CHECK_EQUAL(name1.getCommonLabels(name2), DNSName("powerdns.com"));
  BOOST_CHECK_EQUAL(name2.getCommonLabels(name1), DNSName("powerdns.com"));

  const DNSName name3("www.powerdns.org");
  BOOST_CHECK_EQUAL(name1.getCommonLabels(name3), g_rootdnsname);
  BOOST_CHECK_EQUAL(name2.getCommonLabels(name3), g_rootdnsname);
  BOOST_CHECK_EQUAL(name3.getCommonLabels(name1), g_rootdnsname);
  BOOST_CHECK_EQUAL(name3.getCommonLabels(name2), g_rootdnsname);

  const DNSName name4("WWw.PowErDnS.org");
  BOOST_CHECK_EQUAL(name3.getCommonLabels(name4), name3);
  BOOST_CHECK_EQUAL(name4.getCommonLabels(name3), name4);

  const DNSName name5;
  BOOST_CHECK_EQUAL(name1.getCommonLabels(name5), DNSName());
  BOOST_CHECK_EQUAL(name5.getCommonLabels(name1), DNSName());
}

#if defined(PDNS_AUTH)
BOOST_AUTO_TEST_CASE(test_variantnames) {
  ZoneName zone1("..variant");
  ZoneName zone2("bug.less..variant");
  ZoneName zone3(R"(actually\..not.a.variant)");
  ZoneName zone4(R"(still\\\..not.a.variant)");
  ZoneName zone5(R"(anti-\\..variant)");
  ZoneName zone6(R"(sl\\\\\..a\\\..sh\...overflow)");

  BOOST_CHECK(zone1.hasVariant());
  BOOST_CHECK(zone1.operator const DNSName&().isRoot());

  BOOST_CHECK(zone2.hasVariant());
  BOOST_CHECK_EQUAL(zone2.operator const DNSName&().toString(), "bug.less.");
  BOOST_CHECK_EQUAL(zone2.getVariant(), "variant");

  BOOST_CHECK(!zone3.hasVariant());
  BOOST_CHECK(!zone4.hasVariant());

  BOOST_CHECK(zone5.hasVariant());
  BOOST_CHECK_EQUAL(zone5.operator const DNSName&().toString(), R"(anti-\\.)");
  BOOST_CHECK_EQUAL(zone5.getVariant(), "variant");

  BOOST_CHECK(zone6.hasVariant());
  BOOST_CHECK_EQUAL(zone6.getVariant(), "overflow");

  BOOST_CHECK_THROW(ZoneName zone("variants.r.us..dot..dot...dot....dot.....dots"),std::out_of_range);
}
#endif

BOOST_AUTO_TEST_SUITE_END()
