// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Unit tests for the executor against the mock ddsc: spin_some/spin_once
// semantics, dynamic entity attachment (epoch reconcile), node removal and
// cancel().  No network, fully deterministic.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

#include "mock/mock_ddsc.h"
#include "rcllite/rcllite.hpp"
#include "rcllite_types/rcl_interfaces/parameter.hpp"

namespace {

using namespace std::chrono_literals;
using rcl_interfaces::msg::Parameter;

std::vector<uint8_t> make_parameter_sample(const std::string& name) {
  Parameter m;
  m.name = name;
  rcl::CdrWriter w;
  Parameter::serialize(m, w);
  return w.payload();
}

class ExecutorTest : public ::testing::Test {
 protected:
  void SetUp() override { mock_dds::reset(); }
};

TEST_F(ExecutorTest, SpinSomeWithoutWorkReturnsFalse) {
  auto node = std::make_shared<rcl::Node>("n");
  rcl::Executor exec;
  exec.add_node(node);
  EXPECT_FALSE(exec.spin_some());
}

TEST_F(ExecutorTest, SpinOnceWithZeroTimeoutReturnsFalse) {
  auto node = std::make_shared<rcl::Node>("n");
  rcl::Executor exec;
  exec.add_node(node);
  EXPECT_FALSE(exec.spin_once(0ms));
}

TEST_F(ExecutorTest, DispatchesSampleToSubscription) {
  auto node = std::make_shared<rcl::Node>("n");
  rcl::Executor exec;
  exec.add_node(node);
  std::atomic<int> received{0};
  std::string got;
  node->create_subscription<Parameter>("parameters", [&](const Parameter& m) {
    got = m.name;
    ++received;
  });
  const dds_entity_t reader = mock_dds::last_reader();

  // Entity created after add_node: the executor must reconcile and attach.
  const auto payload = make_parameter_sample("hello");
  mock_dds::enqueue_sample(reader, payload.data(), payload.size());
  EXPECT_TRUE(exec.spin_some());
  EXPECT_EQ(received.load(), 1);
  EXPECT_EQ(got, "hello");

  // Queue drained: no more work.
  EXPECT_FALSE(exec.spin_some());
}

TEST_F(ExecutorTest, LateEntityAttachedOnNextSpin) {
  auto node = std::make_shared<rcl::Node>("n");
  rcl::Executor exec;
  exec.add_node(node);
  EXPECT_FALSE(exec.spin_some());  // no entities yet

  std::atomic<int> received{0};
  node->create_subscription<Parameter>("parameters",
                                       [&](const Parameter&) { ++received; });
  const dds_entity_t reader = mock_dds::last_reader();
  const auto payload = make_parameter_sample("late");
  mock_dds::enqueue_sample(reader, payload.data(), payload.size());
  EXPECT_TRUE(exec.spin_some());
  EXPECT_EQ(received.load(), 1);
}

TEST_F(ExecutorTest, RemoveNodeStopsDispatch) {
  auto node = std::make_shared<rcl::Node>("n");
  rcl::Executor exec;
  std::atomic<int> received{0};
  node->create_subscription<Parameter>("parameters",
                                       [&](const Parameter&) { ++received; });
  const dds_entity_t reader = mock_dds::last_reader();
  exec.add_node(node);
  exec.remove_node(node);

  const auto payload = make_parameter_sample("dropped");
  mock_dds::enqueue_sample(reader, payload.data(), payload.size());
  EXPECT_FALSE(exec.spin_some());
  EXPECT_EQ(received.load(), 0);
}

TEST_F(ExecutorTest, CancelStopsSpin) {
  auto node = std::make_shared<rcl::Node>("n");
  rcl::Executor exec;
  exec.add_node(node);
  std::thread spinner([&exec]() { exec.spin(); });
  std::this_thread::sleep_for(20ms);
  exec.cancel();
  spinner.join();  // must return promptly after cancel
  SUCCEED();
}

TEST_F(ExecutorTest, SubscriptionCallbackErrorIsContained) {
  auto node = std::make_shared<rcl::Node>("n");
  rcl::Executor exec;
  exec.add_node(node);
  std::atomic<int> received{0};
  node->create_subscription<Parameter>("parameters", [&](const Parameter&) {
    ++received;
    if (received.load() == 1) {
      throw std::runtime_error("callback boom");
    }
  });
  const dds_entity_t reader = mock_dds::last_reader();

  const auto bad = make_parameter_sample("first");
  const auto good = make_parameter_sample("second");
  mock_dds::enqueue_sample(reader, bad.data(), bad.size());
  mock_dds::enqueue_sample(reader, good.data(), good.size());
  EXPECT_NO_THROW(exec.spin_some());
  EXPECT_EQ(received.load(), 2);  // the second sample still dispatched
}

}  // namespace
