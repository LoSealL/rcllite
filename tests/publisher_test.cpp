// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Unit tests for publisher-side subscriber awareness against the mock ddsc:
// subscription counting, wait_for_subscribers and the backpressure publish
// overload (hold the message while nobody is matched instead of dropping
// it).  No network, fully deterministic.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "mock/mock_ddsc.h"
#include "rcllite/rcllite.hpp"
#include "rcllite_types/rcl_interfaces/parameter.hpp"

namespace {

using rcl_interfaces::msg::Parameter;
using namespace std::chrono_literals;

class PublisherTest : public ::testing::Test {
 protected:
  void SetUp() override { mock_dds::reset(); }
};

TEST_F(PublisherTest, SubscriptionCountReflectsMatchedReaders) {
  auto node = std::make_shared<rcl::Node>("pub_node");
  auto pub = node->create_publisher<Parameter>("chatter");
  const dds_entity_t writer = mock_dds::last_writer();

  EXPECT_EQ(pub->get_subscription_count(), 0u);
  mock_dds::set_matched_readers(writer, 2);
  EXPECT_EQ(pub->get_subscription_count(), 2u);
}

TEST_F(PublisherTest, WaitForSubscribersTimesOutWithoutAny) {
  auto node = std::make_shared<rcl::Node>("pub_node");
  auto pub = node->create_publisher<Parameter>("chatter");
  EXPECT_FALSE(pub->wait_for_subscribers(30ms));
}

TEST_F(PublisherTest, WaitForSubscribersReturnsOnceMatched) {
  auto node = std::make_shared<rcl::Node>("pub_node");
  auto pub = node->create_publisher<Parameter>("chatter");
  mock_dds::set_matched_readers(mock_dds::last_writer(), 1);
  EXPECT_TRUE(pub->wait_for_subscribers(30ms));
}

TEST_F(PublisherTest, BackpressurePublishHoldsUntilSubscriberAppears) {
  auto node = std::make_shared<rcl::Node>("pub_node");
  auto pub = node->create_publisher<Parameter>("chatter");
  const dds_entity_t writer = mock_dds::last_writer();

  // Discovery completes on another thread while publish is blocked.
  std::thread late_joiner([writer]() {
    std::this_thread::sleep_for(50ms);
    mock_dds::set_matched_readers(writer, 1);
  });

  Parameter msg;
  msg.name = "held";
  EXPECT_TRUE(pub->publish(msg, 10s));
  late_joiner.join();

  const auto& sent = mock_dds::written(writer);
  ASSERT_EQ(sent.size(), 1u);
  rcl::CdrReader r(sent[0]);
  Parameter got;
  ASSERT_TRUE(Parameter::deserialize(got, r));
  EXPECT_EQ(got.name, "held");
}

TEST_F(PublisherTest, BackpressurePublishTimesOutWithoutWriting) {
  auto node = std::make_shared<rcl::Node>("pub_node");
  auto pub = node->create_publisher<Parameter>("chatter");
  const dds_entity_t writer = mock_dds::last_writer();

  Parameter msg;
  msg.name = "kept-by-caller";
  EXPECT_FALSE(pub->publish(msg, 30ms));
  // Nothing reached the wire; the caller still owns the message.
  EXPECT_TRUE(mock_dds::written(writer).empty());
}

TEST_F(PublisherTest, WaitWindowBlocksUntilReaderAcks) {
  auto node = std::make_shared<rcl::Node>("pub_node");
  auto pub = node->create_publisher<Parameter>("chatter",
                                               rcl::QoS().keep_last(2).wait_forever());
  const dds_entity_t writer = mock_dds::last_writer();

  Parameter msg;
  msg.name = "frame";
  EXPECT_TRUE(pub->publish(msg));  // fills the window
  EXPECT_TRUE(pub->publish(msg));
  ASSERT_EQ(mock_dds::written(writer).size(), 2u);

  std::atomic<bool> finished{false};
  std::atomic<bool> sent{false};
  std::thread third([&] {
    sent = pub->publish(msg);
    finished = true;
  });

  // Window full and nobody acked: the write is parked, not dropped.
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(finished.load());

  mock_dds::ack_all(writer);
  third.join();
  EXPECT_TRUE(finished.load());
  EXPECT_TRUE(sent.load());
  EXPECT_EQ(mock_dds::written(writer).size(), 3u);
}

TEST_F(PublisherTest, WaitTimeoutLeavesSampleUnwritten) {
  auto node = std::make_shared<rcl::Node>("pub_node");
  auto pub = node->create_publisher<Parameter>("chatter",
                                               rcl::QoS().keep_last(1).wait_for(30ms));
  const dds_entity_t writer = mock_dds::last_writer();

  Parameter msg;
  msg.name = "frame";
  EXPECT_TRUE(pub->publish(msg));   // fills the single slot
  EXPECT_FALSE(pub->publish(msg));  // 30 ms pass without an ack
  EXPECT_EQ(mock_dds::written(writer).size(), 1u);

  mock_dds::ack_all(writer);
  EXPECT_TRUE(pub->publish(msg));  // window drained, write resumes
  EXPECT_EQ(mock_dds::written(writer).size(), 2u);
}

TEST_F(PublisherTest, WaitPolicyRequiresReliableKeepLast) {
  auto node = std::make_shared<rcl::Node>("pub_node");
  EXPECT_THROW((void)node->create_publisher<Parameter>(
                   "chatter", rcl::QoS().set_best_effort().keep_last(2).wait_forever()),
               rcl::Error);

  rcl::QoS keep_all;
  keep_all.history = rcl::QoS::History::KeepAll;
  keep_all.wait_forever();
  EXPECT_THROW((void)node->create_publisher<Parameter>("chatter", keep_all),
               rcl::Error);
}

TEST_F(PublisherTest, NoWaitPolicyPublishesFreely) {
  auto node = std::make_shared<rcl::Node>("pub_node");
  auto pub = node->create_publisher<Parameter>("chatter");
  const dds_entity_t writer = mock_dds::last_writer();

  Parameter msg;
  msg.name = "burst";
  for (int i = 0; i < 20; ++i) {
    EXPECT_TRUE(pub->publish(msg));
  }
  EXPECT_EQ(mock_dds::written(writer).size(), 20u);
}

}  // namespace
