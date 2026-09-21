// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// End-to-end QoS tests through the real CycloneDDS loopback path (in one
// process, real RTPS on the network interfaces).  These bind real sockets:
// run them manually with
//   bazelisk test //tests:qos_e2e_test
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "rcllite/rcllite.hpp"
#include "rcllite_types/rcl_interfaces/parameter.hpp"

namespace {

using namespace std::chrono_literals;
using rcl_interfaces::msg::Parameter;

TEST(QosE2E, BestEffortDelivery) {
  auto node_a = std::make_shared<rcl::Node>("qos_pub");
  auto node_b = std::make_shared<rcl::Node>("qos_sub");

  std::atomic<int> received{0};
  std::string got;
  node_b->create_subscription<Parameter>(
      "sensor",
      [&](const Parameter& m) {
        got = m.name;
        ++received;
      },
      rcl::QoS::SensorData());
  auto pub = node_a->create_publisher<Parameter>("sensor", rcl::QoS::SensorData());

  rcl::Executor exec;
  exec.add_node(node_a);
  exec.add_node(node_b);
  std::thread spinner([&exec]() { exec.spin(); });

  Parameter m;
  m.name = "best-effort hello";
  for (int i = 0; i < 100 && received.load() == 0; ++i) {
    pub->publish(m);
    std::this_thread::sleep_for(100ms);
  }
  EXPECT_GT(received.load(), 0);
  EXPECT_EQ(got, "best-effort hello");

  exec.cancel();
  spinner.join();
}

TEST(QosE2E, TransientLocalLateJoiner) {
  // Publish one durable sample BEFORE the subscriber exists.
  auto node_a = std::make_shared<rcl::Node>("durable_pub");
  auto pub = node_a->create_publisher<Parameter>(
      "durable", rcl::QoS().set_transient_local().keep_last(1));
  Parameter m;
  m.name = "from the past";
  pub->publish(m);

  auto node_b = std::make_shared<rcl::Node>("durable_sub");
  std::atomic<int> received{0};
  std::string got;
  node_b->create_subscription<Parameter>(
      "durable",
      [&](const Parameter& p) {
        got = p.name;
        ++received;
      },
      rcl::QoS().set_transient_local().keep_last(1));

  rcl::Executor exec;
  exec.add_node(node_b);
  std::thread spinner([&exec]() { exec.spin(); });

  // Transient-local delivers the historical sample once matched; allow time
  // for discovery plus the replay.
  for (int i = 0; i < 150 && received.load() == 0; ++i) {
    std::this_thread::sleep_for(100ms);
  }
  EXPECT_GT(received.load(), 0);
  EXPECT_EQ(got, "from the past");

  exec.cancel();
  spinner.join();
}

}  // namespace
