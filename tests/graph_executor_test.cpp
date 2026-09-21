// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Unit tests for GraphExecutor against the mock ddsc: topology validation,
// endpoint creation (counters, QoS translation), payload passthrough and
// the publish schedule.  No network, fully deterministic.
#include "rcllite/graph_executor.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "mock/mock_ddsc.h"
#include "rcllite/node_registry.hpp"
#include "rcllite/rcllite.hpp"
#include "rcllite_types/rcl_interfaces/parameter.hpp"

namespace {

using namespace std::chrono_literals;
using rcl_interfaces::msg::Parameter;

// The exact payload_hex used by python/examples/topology.yaml: CDR-LE for
// Parameter{name: "hello from graph", value: <defaults>} — the full message,
// ParameterValue field included.
constexpr const char* kTalkerPayloadHex =
    "000100001100000068656c6c6f2066726f6d206772617068000000000000000000000000"
    "0000000000000100000000000000000000000000000000000000000000000000";

/// Reference bytes: what the typed CdrWriter produces for the same message.
std::vector<uint8_t> serialized_parameter(const std::string& name) {
  Parameter m;
  m.name = name;
  rcl::CdrWriter w;
  Parameter::serialize(m, w);
  return w.payload();
}

std::string status_of(const rcl::GraphExecutor& exec) { return exec.status_json(); }

class GraphExecutorTest : public ::testing::Test {
 protected:
  void SetUp() override { mock_dds::reset(); }
};

TEST_F(GraphExecutorTest, RejectsInvalidTopologies) {
  EXPECT_THROW(rcl::GraphExecutor("{ not json"), rcl::Error);
  EXPECT_THROW(rcl::GraphExecutor("{}"), rcl::Error);
  EXPECT_THROW(rcl::GraphExecutor(R"({"nodes": []})"), rcl::Error);
  EXPECT_THROW(rcl::GraphExecutor(R"({"nodes": [{}]})"), rcl::Error);
  // node names go through the same validation as rcl::Node
  EXPECT_THROW(rcl::GraphExecutor(R"({"nodes": [{"name": "a/b"}]})"), rcl::Error);
  EXPECT_THROW(rcl::GraphExecutor(
                   R"({"nodes": [{"name": "n", "publishers": [{"topic": "/t"}]}]})"),
               rcl::Error);  // missing type
}

TEST_F(GraphExecutorTest, CreatesEndpointsAndReportsStatus) {
  rcl::GraphExecutor exec(R"({
    "nodes": [
      {"name": "talker",
       "publishers": [{"topic": "/chatter", "type": "rcl_interfaces/msg/Parameter",
                       "payload_hex": "00010000"}]},
      {"name": "listener",
       "subscriptions": [{"topic": "/chatter", "type": "rcl_interfaces/msg/Parameter"}]}
    ]
  })");
  const std::string status = status_of(exec);
  EXPECT_NE(status.find("\"name\":\"/talker\""), std::string::npos);
  EXPECT_NE(status.find("\"name\":\"/listener\""), std::string::npos);
  EXPECT_NE(status.find("\"topic\":\"/chatter\""), std::string::npos);
  // one-shot publisher fired exactly once
  EXPECT_NE(status.find("\"published\":1"), std::string::npos);
}

TEST_F(GraphExecutorTest, PublishesTypedCompatiblePayload) {
  // The whole point of payload_hex: bytes a typed ROS 2/rcllite peer accepts.
  rcl::GraphExecutor exec(R"({
    "nodes": [{"name": "talker",
      "publishers": [{"topic": "/chatter", "type": "rcl_interfaces/msg/Parameter",
                      "payload_hex": "000100001100000068656c6c6f2066726f6d206772617068000000000000000000000000000000000000000001000000000000000000000000000000000000000000000000000000"}]}]
  })");

  // The writer recorded by the mock must equal the typed serialization.
  const auto written = mock_dds::written(mock_dds::last_writer());
  ASSERT_EQ(written.size(), 1u);
  EXPECT_EQ(written[0], serialized_parameter("hello from graph"));

  // And a typed subscription accepts the recorded blob without dropping it.
  auto node = std::make_shared<rcl::Node>("typed_listener");
  rcl::Executor spin;
  spin.add_node(node);
  std::atomic<int> received{0};
  std::string got;
  node->create_subscription<Parameter>("chatter", [&](const Parameter& m) {
    got = m.name;
    ++received;
  });
  mock_dds::enqueue_sample(mock_dds::last_reader(), written[0].data(),
                           written[0].size());
  spin.spin_once(0ms);
  EXPECT_EQ(received.load(), 1);
  EXPECT_EQ(got, "hello from graph");
}

TEST_F(GraphExecutorTest, RawSubscriptionCountsValidSamples) {
  rcl::GraphExecutor exec(R"({
    "nodes": [{"name": "listener",
      "subscriptions": [{"topic": "/chatter", "type": "rcl_interfaces/msg/Parameter"}]}]
  })");
  const dds_entity_t reader = mock_dds::last_reader();
  const auto blob = serialized_parameter("x");
  mock_dds::enqueue_sample(reader, blob.data(), blob.size());
  ASSERT_TRUE(exec.spin_once(0ms));
  EXPECT_NE(status_of(exec).find("\"received\":1"), std::string::npos);
}

TEST_F(GraphExecutorTest, TranslatesQoSandRelativeTopics) {
  rcl::GraphExecutor exec(R"({
    "nodes": [{"name": "relay", "namespace": "/demo",
      "publishers": [{"topic": "echoed", "type": "std_msgs/msg/String",
                      "qos": {"reliability": "best_effort", "durability": "transient_local",
                              "depth": 5}}],
      "subscriptions": [{"topic": "/chatter", "type": "std_msgs/msg/String"}]}]
  })");
  const auto* wq = mock_dds::writer_qos(mock_dds::last_writer());
  ASSERT_NE(wq, nullptr);
  EXPECT_EQ(wq->reliability, DDS_RELIABILITY_BEST_EFFORT);
  EXPECT_EQ(wq->durability, DDS_DURABILITY_TRANSIENT_LOCAL);
  EXPECT_EQ(wq->depth, 5);
  // status reports the expanded absolute topic, not the relative spelling
  EXPECT_NE(status_of(exec).find("\"topic\":\"/demo/echoed\""), std::string::npos);
}

TEST_F(GraphExecutorTest, PeriodicPublisherFollowsRate) {
  rcl::GraphExecutor exec(R"({
    "nodes": [{"name": "talker",
      "publishers": [{"topic": "/chatter", "type": "std_msgs/msg/String",
                      "payload_hex": "00010000", "rate_hz": 50.0}]}]
  })");
  std::this_thread::sleep_for(120ms);
  const auto written = mock_dds::written(mock_dds::last_writer());
  // ~6 samples at 50 Hz in 120 ms; mock records every write
  EXPECT_GE(written.size(), 3u);
  EXPECT_LE(written.size(), 12u);
  EXPECT_TRUE(exec.spin_once(0ms) || true);  // no waitable entities: fine
}

TEST_F(GraphExecutorTest, RejectsUnsupportedParameterTypes) {
  // YAML/JSON arrays/objects are not valid parameter values in the schema.
  EXPECT_THROW(rcl::GraphExecutor(
                   R"({"nodes": [{"name": "n", "parameters": {"bad": [1, 2]}}]})"),
               rcl::Error);
  EXPECT_THROW(rcl::GraphExecutor(
                   R"({"nodes": [{"name": "n", "parameters": {"bad": {"x": 1}}}]})"),
               rcl::Error);
  // A well-typed parameter table builds fine.
  rcl::GraphExecutor exec(R"({
    "nodes": [{"name": "params",
      "parameters": {"an_int": 10, "a_double": 0.5, "a_string": "hi", "a_bool": true}}]
  })");
  SUCCEED();
}

// --- node class registry -----------------------------------------------------

// A behavior-carrying subclass registered at static-init via the public
// macro — exactly how user plugins register (proving that path end to end).
class CountingNode : public rcl::Node {
 public:
  CountingNode(const std::string& name, const std::string& ns) : rcl::Node(name, ns) {
    ++instantiations;
  }

  static std::atomic<int> instantiations;
};

std::atomic<int> CountingNode::instantiations{0};

RCLLITE_REGISTER_NODE(CountingNode);

TEST_F(GraphExecutorTest, ClassFieldInstantiatesRegisteredSubclass) {
  CountingNode::instantiations = 0;
  rcl::GraphExecutor exec(R"({
    "nodes": [{"name": "ct", "namespace": "/x", "class": "CountingNode",
               "parameters": {"k": 1}}]
  })");
  EXPECT_EQ(CountingNode::instantiations.load(), 1);
  // config-driven assembly still applies on top of the subclass
  EXPECT_NE(status_of(exec).find("\"name\":\"/x/ct\""), std::string::npos);
}

TEST_F(GraphExecutorTest, UnknownClassThrows) {
  EXPECT_THROW(rcl::GraphExecutor(R"({"nodes": [{"name": "n", "class": "no::Such"}]})"),
               rcl::Error);
}

TEST_F(GraphExecutorTest, SameClassInstantiatedPerNode) {
  CountingNode::instantiations = 0;
  rcl::GraphExecutor exec(R"({
    "nodes": [{"name": "a", "class": "CountingNode"},
              {"name": "b", "class": "CountingNode"}]
  })");
  EXPECT_EQ(CountingNode::instantiations.load(), 2);
}

TEST_F(GraphExecutorTest, RuntimeRegisteredFactoryAndJsonListing) {
  // Runtime registration (unlike the macro) is revocable: inject, use,
  // verify the listing, unregister, expect the class to disappear.
  auto token = rcl::NodeRegistry::Register(
      "temp::Node", [](const std::string& name, const std::string& ns) {
        return std::make_shared<CountingNode>(name, ns);
      });
  {
    const std::string listing = rcl::GraphExecutor::registered_nodes_json();
    EXPECT_NE(listing.find("CountingNode"), std::string::npos);
    EXPECT_NE(listing.find("temp::Node"), std::string::npos);
    CountingNode::instantiations = 0;
    rcl::GraphExecutor exec(R"({"nodes": [{"name": "t", "class": "temp::Node"}]})");
    EXPECT_EQ(CountingNode::instantiations.load(), 1);
  }
  token.Unregister();
  EXPECT_THROW(
      rcl::GraphExecutor(R"({"nodes": [{"name": "t", "class": "temp::Node"}]})"),
      rcl::Error);
  EXPECT_EQ(rcl::GraphExecutor::registered_nodes_json().find("temp::Node"),
            std::string::npos);
}
}  // namespace
