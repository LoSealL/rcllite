// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Unit tests for the QoS translation rcl::QoS -> dds_qos_t, observed
// through the mock ddsc at entity creation.  No network.
#include <gtest/gtest.h>

#include "mock/mock_ddsc.h"
#include "rcllite/rcllite.hpp"
#include "rcllite_types/rcl_interfaces/parameter.hpp"

namespace {

using rcl_interfaces::msg::Parameter;

class QosTest : public ::testing::Test {
 protected:
  void SetUp() override { mock_dds::reset(); }
};

TEST_F(QosTest, DefaultProfileTranslates) {
  auto node = std::make_shared<rcl::Node>("n");
  auto pub = node->create_publisher<Parameter>("t");
  const auto* q = mock_dds::writer_qos(mock_dds::last_writer());
  ASSERT_NE(q, nullptr);
  EXPECT_EQ(q->reliability, DDS_RELIABILITY_RELIABLE);
  EXPECT_EQ(q->durability, DDS_DURABILITY_VOLATILE);
  EXPECT_EQ(q->history, DDS_HISTORY_KEEP_LAST);
  EXPECT_EQ(q->depth, 10);
}

TEST_F(QosTest, SensorDataProfileTranslates) {
  auto node = std::make_shared<rcl::Node>("n");
  auto pub = node->create_publisher<Parameter>("t", rcl::QoS::SensorData());
  const auto* q = mock_dds::writer_qos(mock_dds::last_writer());
  ASSERT_NE(q, nullptr);
  EXPECT_EQ(q->reliability, DDS_RELIABILITY_BEST_EFFORT);
  EXPECT_EQ(q->history, DDS_HISTORY_KEEP_LAST);
  EXPECT_EQ(q->depth, 5);
}

TEST_F(QosTest, TransientLocalKeepLastOneTranslates) {
  auto node = std::make_shared<rcl::Node>("n");
  auto pub = node->create_publisher<Parameter>(
      "t", rcl::QoS().set_transient_local().keep_last(1));
  const auto* q = mock_dds::writer_qos(mock_dds::last_writer());
  ASSERT_NE(q, nullptr);
  EXPECT_EQ(q->durability, DDS_DURABILITY_TRANSIENT_LOCAL);
  EXPECT_EQ(q->history, DDS_HISTORY_KEEP_LAST);
  EXPECT_EQ(q->depth, 1);
}

TEST_F(QosTest, BestEffortKeepLastZeroTranslates) {
  auto node = std::make_shared<rcl::Node>("n");
  auto pub =
      node->create_publisher<Parameter>("t", rcl::QoS().set_best_effort().keep_last(0));
  const auto* q = mock_dds::writer_qos(mock_dds::last_writer());
  ASSERT_NE(q, nullptr);
  EXPECT_EQ(q->reliability, DDS_RELIABILITY_BEST_EFFORT);
  EXPECT_EQ(q->depth, 0);
}

TEST_F(QosTest, SubscriptionQoSTranslates) {
  auto node = std::make_shared<rcl::Node>("n");
  node->create_subscription<Parameter>(
      "t", [](const Parameter&) {}, rcl::QoS::Services());
  const auto* q = mock_dds::reader_qos(mock_dds::last_reader());
  ASSERT_NE(q, nullptr);
  EXPECT_EQ(q->reliability, DDS_RELIABILITY_RELIABLE);
  EXPECT_EQ(q->history, DDS_HISTORY_KEEP_LAST);
  EXPECT_EQ(q->depth, 10);
}

}  // namespace
