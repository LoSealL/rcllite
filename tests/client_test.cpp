// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Unit tests for the service client against the mock ddsc: asynchronous
// request/response completion, sequence correlation, response filtering and
// the timeout paths.  No network, fully deterministic.
#include <gtest/gtest.h>

#include <chrono>

#include "mock/mock_ddsc.h"
#include "rcllite/rcllite.hpp"
#include "rcllite_types/rcl_interfaces/get_parameters.hpp"

namespace {

using rcl_interfaces::srv::GetParameters;
using namespace std::chrono_literals;

class ClientTest : public ::testing::Test {
 protected:
  void SetUp() override { mock_dds::reset(); }
};

TEST_F(ClientTest, AsyncSendRequestDeliversResponse) {
  auto node = std::make_shared<rcl::Node>("client_node");
  rcl::Executor exec;
  exec.add_node(node);
  auto client = node->create_client<GetParameters>("get_parameters");
  const dds_entity_t request_writer = mock_dds::last_writer();
  const dds_entity_t response_reader = mock_dds::last_reader();

  GetParameters::Request req;
  req.names = {"speed", "name"};
  bool cb_called = false;
  int64_t got_value = 0;
  const int64_t seq =
      client->async_send_request(req, [&](const GetParameters::Response& r) {
        cb_called = true;
        got_value = r.values.empty() ? -1 : r.values[0].integer_value;
      });

  // The request carries the service header followed by the request CDR.
  const auto& sent = mock_dds::written(request_writer);
  ASSERT_EQ(sent.size(), 1u);
  rcl::CdrReader rr(sent[0]);
  const rcl::ServiceHeader h = rcl::read_service_header(rr);
  GetParameters::Request sent_req;
  ASSERT_TRUE(GetParameters::Request::deserialize(sent_req, rr));
  EXPECT_EQ(h.sequence, seq);
  EXPECT_NE(h.client_id, uint64_t{0});
  ASSERT_EQ(sent_req.names.size(), 2u);
  EXPECT_EQ(sent_req.names[0], "speed");

  // Craft the matching response and deliver it on the response reader.
  rcl::CdrWriter w;
  rcl::write_service_header(w, h);
  GetParameters::Response resp;
  rcl_interfaces::msg::ParameterValue v;
  v.type = 2;  // PARAMETER_INTEGER
  v.integer_value = 42;
  resp.values.push_back(v);
  GetParameters::Response::serialize(resp, w);
  mock_dds::enqueue_sample(response_reader, w.payload().data(), w.payload().size());

  EXPECT_TRUE(exec.spin_some());
  EXPECT_TRUE(cb_called);
  EXPECT_EQ(got_value, 42);
}

TEST_F(ClientTest, IgnoresResponsesFromOtherClients) {
  auto node = std::make_shared<rcl::Node>("client_node");
  rcl::Executor exec;
  exec.add_node(node);
  auto client = node->create_client<GetParameters>("get_parameters");
  const dds_entity_t response_reader = mock_dds::last_reader();

  GetParameters::Request req;
  bool cb_called = false;
  const int64_t seq = client->async_send_request(
      req, [&](const GetParameters::Response&) { cb_called = true; });

  // A response with a foreign client id must be silently ignored...
  rcl::CdrWriter foreign;
  rcl::write_service_header(foreign, rcl::ServiceHeader{0xDEADBEEF, seq});
  GetParameters::Response resp;
  rcl_interfaces::msg::ParameterValue v;
  v.type = 2;
  v.integer_value = 1;
  resp.values.push_back(v);
  GetParameters::Response::serialize(resp, foreign);
  mock_dds::enqueue_sample(response_reader, foreign.payload().data(),
                           foreign.payload().size());
  exec.spin_some();
  EXPECT_FALSE(cb_called);

  // ... and the pending request must still complete on the real response
  // (read the actual client id back from the written request).
  const auto& sent = mock_dds::written(mock_dds::last_writer());
  rcl::CdrReader rr(sent[0]);
  const rcl::ServiceHeader h = rcl::read_service_header(rr);
  rcl::CdrWriter match;
  rcl::write_service_header(match, h);
  GetParameters::Response ok_resp;
  ok_resp.values.push_back(v);
  GetParameters::Response::serialize(ok_resp, match);
  mock_dds::enqueue_sample(response_reader, match.payload().data(),
                           match.payload().size());
  EXPECT_TRUE(exec.spin_some());
  EXPECT_TRUE(cb_called);
}

TEST_F(ClientTest, CallWithoutResponseTimesOut) {
  auto node = std::make_shared<rcl::Node>("client_node");
  auto client = node->create_client<GetParameters>("get_parameters");

  GetParameters::Request req;
  try {
    client->call(req, 50ms);
    FAIL() << "expected rcl::Error";
  } catch (const rcl::Error& e) {
    EXPECT_EQ(e.status(), vila::ErrorCode::wait_time_out);
  }
}

TEST_F(ClientTest, WaitForServiceTimesOutWithoutServer) {
  auto node = std::make_shared<rcl::Node>("client_node");
  auto client = node->create_client<GetParameters>("no_such_service");
  EXPECT_FALSE(client->wait_for_service(30ms));
}

TEST_F(ClientTest, WaitForServiceReturnsOnceMatched) {
  auto node = std::make_shared<rcl::Node>("client_node");
  auto client = node->create_client<GetParameters>("get_parameters");
  mock_dds::set_matched_writers(mock_dds::last_reader(), 1);
  EXPECT_TRUE(client->wait_for_service(30ms));
}

}  // namespace
