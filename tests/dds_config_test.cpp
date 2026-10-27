// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "rcllite/dds/entities.hpp"
#include "rcllite/exception.hpp"

namespace {

using rcl::dds::Context;
using rcl::dds::tcp_config_xml;

TEST(DdsConfig, TcpConfigXml) {
  const std::string xml = tcp_config_xml(7606, {"127.0.0.1:7601", "10.0.0.5:7600"});
  EXPECT_NE(xml.find("<Transport>tcp</Transport>"), std::string::npos);
  EXPECT_NE(xml.find("<Port>7606</Port>"), std::string::npos);
  EXPECT_NE(xml.find("<Peer Address=\"127.0.0.1:7601\"/>"), std::string::npos);
  EXPECT_NE(xml.find("<Peer Address=\"10.0.0.5:7600\"/>"), std::string::npos);
  EXPECT_NE(xml.find("<AllowMulticast>false</AllowMulticast>"), std::string::npos);
  // Without an interface pin no Interfaces section is emitted.
  EXPECT_EQ(xml.find("Interfaces"), std::string::npos);
  EXPECT_EQ(xml.find("address="),
            std::string::npos);  // lowercase attr would not match rmw
}

TEST(DdsConfig, TcpConfigXmlInterfacePin) {
  const std::string xml = tcp_config_xml(7606, {}, "127.0.0.1");
  EXPECT_NE(xml.find("<Interfaces><NetworkInterface address=\"127.0.0.1\"/>"
                     "</Interfaces>"),
            std::string::npos);
  // The pin rides inside General, before the Discovery section.
  EXPECT_LT(xml.find("Interfaces"), xml.find("Discovery"));
}

TEST(DdsConfig, TcpConfigXmlSelfDialsWhenPeersEmpty) {
  // Zero peers must still emit one live Peer: an empty <Peers> element is
  // "unconfigured" to CycloneDDS, which then dials dead well-known SPDP
  // ports on localhost and starves discovery — a pure TCP acceptor would
  // never answer SPDP/SEDP to peers that dialed in.
  const std::string xml = tcp_config_xml(7613, {});
  EXPECT_NE(xml.find("<Peer Address=\"127.0.0.1:7613\"/>"), std::string::npos);
  // A pinned interface rides along into the self-dial address.
  const std::string lan = tcp_config_xml(7613, {}, "192.168.3.101");
  EXPECT_NE(lan.find("<Peer Address=\"192.168.3.101:7613\"/>"), std::string::npos);
}

TEST(DdsConfig, TcpConfigXmlNoSelfDialWithPeers) {
  // With real peers configured the fallback never triggers, so the own port
  // must not appear as a peer.
  const std::string xml = tcp_config_xml(7606, {"127.0.0.1:7601"});
  EXPECT_NE(xml.find("<Peer Address=\"127.0.0.1:7601\"/>"), std::string::npos);
  EXPECT_EQ(xml.find("127.0.0.1:7606"), std::string::npos);
}

TEST(DdsConfig, ContextConfigureLifecycle) {
  // configure() before the first instantiation installs the overrides ...
  Context::configure(std::optional<uint32_t>(7), tcp_config_xml(7606, {}));
  const Context& ctx = Context::instance();
  EXPECT_EQ(ctx.domain_id(), 7u);
  EXPECT_EQ(ctx.config_xml(), tcp_config_xml(7606, {}));
  // ... and after it, a second configure is an error, not a silent no-op.
  EXPECT_THROW(Context::configure(std::optional<uint32_t>(9), ""), rcl::Error);
}

}  // namespace
