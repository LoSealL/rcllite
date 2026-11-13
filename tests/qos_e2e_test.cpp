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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

TEST(QosE2E, BoundedReaderStallsPublisher) {
  // One-process CycloneDDS delivers synchronously inside dds_write, so a full
  // bounded reader blocks the next publish call itself (across processes it
  // stalls the receive thread and the writer's ack wait instead).
  auto node_a = std::make_shared<rcl::Node>("bp_pub");
  auto node_b = std::make_shared<rcl::Node>("bp_sub");

  std::atomic<int> received{0};
  node_b->create_subscription<Parameter>(
      "bp", [&](const Parameter&) { ++received; }, rcl::QoS().refuse_when_full(1));
  auto pub = node_a->create_publisher<Parameter>("bp", rcl::QoS().keep_last(2));

  ASSERT_TRUE(pub->wait_for_subscribers(10s));

  Parameter m;
  m.name = "frame";
  EXPECT_TRUE(pub->publish(m));  // fills the reader

  // The next sample has nowhere to go: the write parks instead of overwriting.
  std::atomic<bool> written{false};
  std::thread second([&]() { written = pub->publish(m); });
  std::this_thread::sleep_for(300ms);
  EXPECT_FALSE(written.load());

  // Draining the reader releases the parked write.
  rcl::Executor exec;
  exec.add_node(node_b);
  std::thread spinner([&exec]() { exec.spin(); });
  second.join();
  EXPECT_TRUE(written.load());
  for (int i = 0; i < 100 && received.load() == 0; ++i) {
    std::this_thread::sleep_for(100ms);
  }
  EXPECT_GT(received.load(), 0);

  exec.cancel();
  spinner.join();
}

TEST(QosE2E, BoundedReaderPacesLosslessStream) {
  // Unpaced burst publisher vs a 20 ms/frame consumer behind refuse_when_full:
  // the stream must arrive complete and in order (no overwrite drops), and the
  // publish loop must be dragged to the consumer's pace.
  auto node_a = std::make_shared<rcl::Node>("pace_pub");
  auto node_b = std::make_shared<rcl::Node>("pace_sub");

  std::mutex gotMtx;
  std::vector<std::string> got;
  node_b->create_subscription<Parameter>(
      "pace",
      [&](const Parameter& m) {
        std::this_thread::sleep_for(20ms);
        std::lock_guard<std::mutex> lock(gotMtx);
        got.push_back(m.name);
      },
      rcl::QoS().refuse_when_full(4));
  auto pub = node_a->create_publisher<Parameter>("pace", rcl::QoS().keep_last(2));
  ASSERT_TRUE(pub->wait_for_subscribers(10s));

  constexpr int kFrames = 50;
  rcl::Executor exec;
  exec.add_node(node_b);
  std::thread spinner([&exec]() { exec.spin(); });

  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < kFrames; ++i) {
    Parameter m;
    m.name = "f" + std::to_string(i);
    EXPECT_TRUE(pub->publish(m));
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;

  // Give the tail frames time to drain, then compare the full sequence.
  for (int i = 0; i < 100; ++i) {
    std::lock_guard<std::mutex> lock(gotMtx);
    if (got.size() == static_cast<size_t>(kFrames)) {
      break;
    }
    std::this_thread::sleep_for(20ms);
  }
  exec.cancel();
  spinner.join();

  std::vector<std::string> want;
  for (int i = 0; i < kFrames; ++i) {
    want.push_back("f" + std::to_string(i));
  }
  std::lock_guard<std::mutex> lock(gotMtx);
  EXPECT_EQ(got, want) << "stream lost or reordered samples";
  using secs = std::chrono::duration<double>;
  // Consumer pace kFrames*20ms; without the park the burst finishes in ms.
  EXPECT_GE(secs(elapsed).count(), kFrames * 0.02 * 0.9);
  EXPECT_LT(secs(elapsed).count(), kFrames * 0.02 + 5.0) << "publish stalled beyond the consumer pace";
}

}  // namespace
