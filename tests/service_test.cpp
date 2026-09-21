// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Unit tests for service servers, publishers and subscription input
// validation against the mock ddsc.  No network, fully deterministic.
#include <gtest/gtest.h>

#include <stdexcept>

#include "mock/mock_ddsc.h"
#include "rcllite/rcllite.hpp"
#include "rcllite_types/rcl_interfaces/get_parameters.hpp"
#include "rcllite_types/rcl_interfaces/parameter.hpp"

namespace {

using rcl_interfaces::msg::Parameter;
using rcl_interfaces::srv::GetParameters;

class ServiceTest : public ::testing::Test {
 protected:
  void SetUp() override { mock_dds::reset(); }
};

TEST_F(ServiceTest, RequestDispatchedAndReplyEchoesHeader) {
  auto node = std::make_shared<rcl::Node>("server_node");
  rcl::Executor exec;
  exec.add_node(node);
  node->create_service<GetParameters>("get_parameters",
                                      [](const GetParameters::Request& req) {
                                        GetParameters::Response resp;
                                        for (const auto& name : req.names) {
                                          rcl_interfaces::msg::ParameterValue v;
                                          v.type = 4;  // PARAMETER_STRING
                                          v.string_value = "echo:" + name;
                                          resp.values.push_back(v);
                                        }
                                        return resp;
                                      });
  // Service ctor order: request reader first, then the reply writer.
  const dds_entity_t request_reader = mock_dds::last_reader();
  const dds_entity_t reply_writer = mock_dds::last_writer();

  rcl::CdrWriter w;
  rcl::write_service_header(w, rcl::ServiceHeader{0x1234, 7});
  GetParameters::Request req;
  req.names = {"speed", "name"};
  GetParameters::Request::serialize(req, w);
  mock_dds::enqueue_sample(request_reader, w.payload().data(), w.payload().size());

  EXPECT_TRUE(exec.spin_some());

  const auto& replies = mock_dds::written(reply_writer);
  ASSERT_EQ(replies.size(), 1u);
  rcl::CdrReader rr(replies[0]);
  const rcl::ServiceHeader h = rcl::read_service_header(rr);
  EXPECT_EQ(h.client_id, uint64_t{0x1234});
  EXPECT_EQ(h.sequence, 7);
  GetParameters::Response resp;
  ASSERT_TRUE(GetParameters::Response::deserialize(resp, rr));
  ASSERT_EQ(resp.values.size(), 2u);
  EXPECT_EQ(resp.values[0].string_value, "echo:speed");
  EXPECT_EQ(resp.values[1].string_value, "echo:name");
}

TEST_F(ServiceTest, CallbackErrorIsContained) {
  auto node = std::make_shared<rcl::Node>("server_node");
  rcl::Executor exec;
  exec.add_node(node);
  int calls = 0;
  node->create_service<GetParameters>(
      "get_parameters", [&](const GetParameters::Request&) -> GetParameters::Response {
        ++calls;
        throw std::runtime_error("handler boom");
      });
  const dds_entity_t request_reader = mock_dds::last_reader();

  rcl::CdrWriter w;
  rcl::write_service_header(w, rcl::ServiceHeader{1, 1});
  GetParameters::Request::serialize(GetParameters::Request{}, w);
  mock_dds::enqueue_sample(request_reader, w.payload().data(), w.payload().size());

  EXPECT_NO_THROW(exec.spin_some());
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(mock_dds::written(mock_dds::last_writer()).empty());
}

TEST_F(ServiceTest, PublishFailureThrowsInternalError) {
  auto node = std::make_shared<rcl::Node>("pub_node");
  auto pub = node->create_publisher<Parameter>("parameter_events_local");
  Parameter m;
  m.name = "x";

  mock_dds::set_write_result(DDS_RETCODE_ERROR);
  try {
    pub->publish(m);
    FAIL() << "expected rcl::Error";
  } catch (const rcl::Error& e) {
    EXPECT_EQ(e.status(), vila::ErrorCode::internal_error);
  }

  mock_dds::set_write_result(DDS_RETCODE_OK);
  pub->publish(m);
  EXPECT_EQ(mock_dds::written(mock_dds::last_writer()).size(), 1u);
}

TEST_F(ServiceTest, SubscriptionDropsMalformedSample) {
  auto node = std::make_shared<rcl::Node>("sub_node");
  rcl::Executor exec;
  exec.add_node(node);
  std::atomic<int> received{0};
  std::string got;
  node->create_subscription<Parameter>("parameters", [&](const Parameter& m) {
    got = m.name;
    ++received;
  });
  const dds_entity_t reader = mock_dds::last_reader();

  // Valid encapsulation header followed by an impossible string length.
  const std::vector<uint8_t> garbage{0x00, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
  mock_dds::enqueue_sample(reader, garbage.data(), garbage.size());

  rcl::CdrWriter w;
  Parameter ok;
  ok.name = "fine";
  Parameter::serialize(ok, w);
  mock_dds::enqueue_sample(reader, w.payload().data(), w.payload().size());

  EXPECT_TRUE(exec.spin_some());
  EXPECT_EQ(received.load(), 1);
  EXPECT_EQ(got, "fine");
}

}  // namespace
