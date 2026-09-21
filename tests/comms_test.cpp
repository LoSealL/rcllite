// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Full-stack integration test: two nodes in one process communicate through
// the real CycloneDDS loopback path (raw serdata -> RTPS -> raw serdata),
// exercising pub/sub, services and the parameter services end to end using
// the official ROS 2 interface types.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "rcllite/rcllite.hpp"
#include "rcllite_types/rcl_interfaces/get_parameters.hpp"
#include "rcllite_types/rcl_interfaces/parameter.hpp"
#include "rcllite_types/rcl_interfaces/set_parameters.hpp"
#include "rcllite_types/rosgraph_msgs/clock.hpp"

namespace {

using namespace std::chrono_literals;
using rcl_interfaces::msg::Parameter;
using rcl_interfaces::srv::GetParameters;
using rcl_interfaces::srv::SetParameters;
using rosgraph_msgs::msg::Clock;

TEST(Comms, EndToEnd) {
  auto node_a = std::make_shared<rcl::Node>("test_node_a");
  auto node_b = std::make_shared<rcl::Node>("test_node_b");

  // --- pub/sub --------------------------------------------------------------
  std::atomic<int> received{0};
  std::string last_name;
  node_b->create_subscription<Parameter>("chatter", [&](const Parameter& m) {
    last_name = m.name;
    ++received;
  });
  auto pub = node_a->create_publisher<Parameter>("chatter");

  rcl::Executor executor;
  executor.add_node(node_a);
  executor.add_node(node_b);
  std::thread spinner([&executor]() { executor.spin(); });

  // wait for discovery to match the endpoints, then publish
  for (int i = 0; i < 100 && received.load() == 0; ++i) {
    Parameter m;
    m.name = "hello rcllite";
    pub->publish(m);
    std::this_thread::sleep_for(100ms);
  }
  EXPECT_GT(received.load(), 0);
  EXPECT_EQ(last_name, "hello rcllite");

  // --- complex message roundtrip (nested struct) ------------------------------
  std::atomic<int> clock_rx{0};
  int32_t stamp_sec = 0;
  node_b->create_subscription<Clock>("pose", [&](const Clock& c) {
    stamp_sec = c.clock.sec;
    ++clock_rx;
  });
  auto clock_pub = node_a->create_publisher<Clock>("pose");
  for (int i = 0; i < 100 && clock_rx.load() == 0; ++i) {
    Clock c;
    c.clock.sec = 1726000000;
    c.clock.nanosec = 42;
    clock_pub->publish(c);
    std::this_thread::sleep_for(100ms);
  }
  EXPECT_GT(clock_rx.load(), 0);
  EXPECT_EQ(stamp_sec, 1726000000);

  // --- service ----------------------------------------------------------------
  node_a->create_service<GetParameters>(
      "get_parameters", [](const GetParameters::Request& req) {
        GetParameters::Response resp;
        for (size_t i = 0; i < req.names.size(); ++i) {
          rcl_interfaces::msg::ParameterValue v;
          v.type = 2;  // PARAMETER_INTEGER
          v.integer_value = static_cast<int64_t>(100 + i);
          resp.values.push_back(v);
        }
        return resp;
      });
  auto client = node_b->create_client<GetParameters>("get_parameters");
  ASSERT_TRUE(client->wait_for_service(10s));

  GetParameters::Request req;
  req.names = {"a", "b"};
  auto resp = client->call(req, 10s);
  ASSERT_EQ(resp.values.size(), 2u);
  EXPECT_EQ(resp.values[0].integer_value, 100);
  EXPECT_EQ(resp.values[1].integer_value, 101);

  // multiple sequential calls
  for (int64_t i = 1; i <= 5; ++i) {
    GetParameters::Request r2;
    r2.names = {"only"};
    auto r = client->call(r2, 10s);
    ASSERT_EQ(r.values.size(), 1u);
    EXPECT_EQ(r.values[0].integer_value, 100);
  }

  // --- parameters (remote set + get through the rcl_interfaces services) ------
  node_a->declare_parameter("speed", int64_t{7});
  node_a->declare_parameter("name", std::string("rcllite"));

  auto set_client = node_b->create_client<SetParameters>("/test_node_a/set_parameters");
  ASSERT_TRUE(set_client->wait_for_service(10s));

  SetParameters::Request set_req;
  Parameter p;
  p.name = "speed";
  p.value.type = 2;  // PARAMETER_INTEGER
  p.value.integer_value = 99;
  set_req.parameters.push_back(p);
  auto set_resp = set_client->call(set_req, 10s);
  EXPECT_EQ(set_resp.results.size(), 1u);
  EXPECT_TRUE(set_resp.results[0].successful);

  EXPECT_EQ(node_a->get_parameter<int64_t>("speed"), 99);

  auto get_client = node_b->create_client<GetParameters>("/test_node_a/get_parameters");
  ASSERT_TRUE(get_client->wait_for_service(10s));
  GetParameters::Request get_req;
  get_req.names = {"speed", "name"};
  auto get_resp = get_client->call(get_req, 10s);
  EXPECT_EQ(get_resp.values.size(), 2u);
  EXPECT_EQ(get_resp.values[0].type, 2);
  EXPECT_EQ(get_resp.values[0].integer_value, 99);
  EXPECT_EQ(get_resp.values[1].string_value, "rcllite");

  // setting an undeclared parameter is rejected, like rcl's default
  SetParameters::Request bad_req;
  bad_req.parameters.push_back(p);
  bad_req.parameters[0].name = "undeclared";
  auto bad_resp = set_client->call(bad_req, 10s);
  EXPECT_FALSE(bad_resp.results[0].successful);

  // --- namespace remapping -----------------------------------------------------
  std::atomic<int> ns_received{0};
  node_b->create_subscription<Parameter>("/robot/ns_chatter",
                                         [&](const Parameter&) { ++ns_received; });
  auto ns_node = std::make_shared<rcl::Node>("ns_talker", "/robot");
  executor.add_node(ns_node);
  auto ns_pub = ns_node->create_publisher<Parameter>("ns_chatter");
  for (int i = 0; i < 100 && ns_received.load() == 0; ++i) {
    Parameter m;
    m.name = "namespaced";
    ns_pub->publish(m);
    std::this_thread::sleep_for(100ms);
  }
  EXPECT_GT(ns_received.load(), 0);

  executor.cancel();
  spinner.join();
}

}  // namespace
