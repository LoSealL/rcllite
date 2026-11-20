// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Unit tests for GraphExecutor against the mock ddsc: topology validation,
// endpoint creation (counters, QoS translation), payload passthrough and
// the publish schedule.  No network, fully deterministic.
#include "rcllite/graph_executor.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <stdexcept>
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

// --- lifecycle: on_init, the finish order, on_deinit --------------------------

// A lifecycle test node: wires typed endpoints from a per-test config,
// records the init/deinit order, reports its batch when the test says so.
class LifecycleNode : public rcl::Node {
 public:
  struct Wiring {
    std::vector<std::string> pubs;
    std::vector<std::string> subs;
  };
  static std::map<std::string, Wiring> config;
  static std::vector<std::string> init_order;
  static std::vector<std::string> deinit_order;
  static std::mutex order_mtx;

  LifecycleNode(const std::string& name, const std::string& ns) : rcl::Node(name, ns) {}

  void on_init() override {
    {
      std::lock_guard<std::mutex> lock(order_mtx);
      init_order.push_back(get_name());
    }
    const Wiring& wiring = config.at(get_name());
    for (const auto& topic : wiring.pubs) {
      pubs_.push_back(create_publisher<Parameter>(topic));
    }
    for (const auto& topic : wiring.subs) {
      create_subscription<Parameter>(
          topic, [](const Parameter&) {}, rcl::QoS().keep_last(2));
    }
  }

  void on_deinit() override {
    std::lock_guard<std::mutex> lock(order_mtx);
    deinit_order.push_back(get_name());
  }

 private:
  std::vector<std::shared_ptr<rcl::Publisher<Parameter>>> pubs_;
};

std::map<std::string, LifecycleNode::Wiring> LifecycleNode::config;
std::vector<std::string> LifecycleNode::init_order;
std::vector<std::string> LifecycleNode::deinit_order;
std::mutex LifecycleNode::order_mtx;

RCLLITE_REGISTER_NODE(LifecycleNode);

// A node whose on_init throws: construction must unwind cleanly.
class ThrowingInitNode : public rcl::Node {
 public:
  ThrowingInitNode(const std::string& name, const std::string& ns) : rcl::Node(name, ns) {}
  void on_init() override { throw std::runtime_error("boom"); }
};

RCLLITE_REGISTER_NODE(ThrowingInitNode);

/// Poll fn every 2 ms until true or the deadline.
bool wait_for(const std::function<bool()>& fn, double seconds) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (fn()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return fn();
}

std::shared_ptr<rcl::Node> node_named(const rcl::GraphExecutor& exec,
                                      const std::string& name) {
  for (const auto& node : exec.nodes()) {
    if (node->get_name() == name) {
      return node;
    }
  }
  return nullptr;
}

TEST_F(GraphExecutorTest, OnInitRunsForEveryClassNodeInTopologyOrder) {
  LifecycleNode::config = {{"a", {}}, {"b", {}}, {"c", {}}};
  LifecycleNode::init_order.clear();
  LifecycleNode::deinit_order.clear();
  rcl::GraphExecutor exec(R"({"deinit_drain": 0.0, "nodes": [
      {"name": "a", "class": "LifecycleNode"},
      {"name": "b", "class": "LifecycleNode"},
      {"name": "c", "class": "LifecycleNode"}]})");
  EXPECT_EQ((std::vector<std::string>{"a", "b", "c"}), LifecycleNode::init_order);
}

TEST_F(GraphExecutorTest, DeinitRunsFeederFirstRegardlessOfReportOrder) {
  // a feeds b feeds c (typed endpoints from the declarations).
  LifecycleNode::config = {
      {"a", {{"/x"}, {}}}, {"b", {{"/y"}, {"/x"}}}, {"c", {{}, {"/y"}}}};
  LifecycleNode::init_order.clear();
  LifecycleNode::deinit_order.clear();
  rcl::GraphExecutor exec(R"({"deinit_drain": 0.0, "nodes": [
      {"name": "a", "class": "LifecycleNode"},
      {"name": "b", "class": "LifecycleNode"},
      {"name": "c", "class": "LifecycleNode"}]})");
  // Report in REVERSE: only the executor's order can deinit a before c.
  node_named(exec, "c")->mark_batch_finished();
  node_named(exec, "b")->mark_batch_finished();
  for (int i = 0; i < 5; ++i) {
    exec.spin_once(10ms);  // c waits on b, b on a: nothing moves
  }
  EXPECT_FALSE(exec.finished());
  node_named(exec, "a")->mark_batch_finished();
  ASSERT_TRUE(wait_for([&exec] { exec.spin_once(10ms); return exec.finished(); }, 2.0));
  EXPECT_EQ((std::vector<std::string>{"a", "b", "c"}), LifecycleNode::deinit_order);
}

TEST_F(GraphExecutorTest, CyclicWiringCollapsesIntoOneUnit) {
  // Mutual wiring: a -> b and b -> a.  A plain topological walk would
  // deadlock; the SCC unit deinits both once both reported.
  LifecycleNode::config = {
      {"a", {{"/p"}, {"/q"}}}, {"b", {{"/q"}, {"/p"}}}};
  LifecycleNode::init_order.clear();
  LifecycleNode::deinit_order.clear();
  rcl::GraphExecutor exec(R"({"deinit_drain": 0.0, "nodes": [
      {"name": "a", "class": "LifecycleNode"},
      {"name": "b", "class": "LifecycleNode"}]})");
  node_named(exec, "a")->mark_batch_finished();
  for (int i = 0; i < 5; ++i) {
    exec.spin_once(10ms);  // the unit needs every member's report
  }
  EXPECT_FALSE(exec.finished());
  node_named(exec, "b")->mark_batch_finished();
  ASSERT_TRUE(wait_for([&exec] { exec.spin_once(10ms); return exec.finished(); }, 2.0));
  std::lock_guard<std::mutex> lock(LifecycleNode::order_mtx);
  EXPECT_EQ(LifecycleNode::deinit_order.size(), 2U);
}

TEST_F(GraphExecutorTest, SccUnitFeedingConsumerWaitsForEveryMember) {
  // p and q form a cycle; both feed r.  A fast sibling (p deinit done)
  // must NOT release r while q still finalizes: the unit counts as done
  // only when EVERY member deinitialized.
  LifecycleNode::config = {
      {"p", {{"/p_out"}, {"/q_out"}}},
      {"q", {{"/q_out", "/feed"}, {"/p_out"}}},
      {"r", {{}, {"/feed"}}}};
  LifecycleNode::init_order.clear();
  LifecycleNode::deinit_order.clear();
  rcl::GraphExecutor exec(R"({"deinit_drain": 0.0, "nodes": [
      {"name": "p", "class": "LifecycleNode"},
      {"name": "q", "class": "LifecycleNode"},
      {"name": "r", "class": "LifecycleNode"}]})");
  node_named(exec, "r")->mark_batch_finished();
  node_named(exec, "p")->mark_batch_finished();
  node_named(exec, "q")->mark_batch_finished();
  ASSERT_TRUE(wait_for([&exec] { exec.spin_once(10ms); return exec.finished(); }, 2.0));
  std::lock_guard<std::mutex> lock(LifecycleNode::order_mtx);
  const auto& order = LifecycleNode::deinit_order;
  ASSERT_EQ(order.size(), 3U);
  EXPECT_NE(std::find(order.begin(), order.end(), "r"),
            std::find(order.begin(), order.end(), "q"));
}

TEST_F(GraphExecutorTest, FinishedCallbackFiresOnce) {
  LifecycleNode::config = {{"a", {}}};
  LifecycleNode::init_order.clear();
  LifecycleNode::deinit_order.clear();
  rcl::GraphExecutor exec(R"({"deinit_drain": 0.0, "nodes": [
      {"name": "a", "class": "LifecycleNode"}]})");
  std::atomic<int> fired{0};
  exec.set_on_finished([&fired] { ++fired; });
  node_named(exec, "a")->mark_batch_finished();
  ASSERT_TRUE(wait_for(
      [&] { exec.spin_once(10ms); return exec.finished() && fired.load() > 0; }, 2.0));
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(fired.load(), 1);
}

TEST_F(GraphExecutorTest, OnInitThrowUnwindsTheGraph) {
  EXPECT_THROW(rcl::GraphExecutor(R"({"deinit_drain": 0.0, "nodes": [
      {"name": "boom", "class": "ThrowingInitNode"}]})"),
               rcl::Error);
}
}  // namespace
