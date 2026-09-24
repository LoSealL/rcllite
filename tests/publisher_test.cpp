// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Unit tests for publisher-side subscriber awareness against the mock ddsc:
// subscription counting, wait_for_subscribers and the backpressure publish
// overload (hold the message while nobody is matched instead of dropping
// it).  No network, fully deterministic.
#include <gtest/gtest.h>

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

}  // namespace
