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
  EXPECT_EQ(xml.find("address="),
            std::string::npos);  // lowercase attr would not match rmw
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
