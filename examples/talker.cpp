// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Minimal publisher demo using the official rcl_interfaces/Parameter type:
// `ros2 topic echo /chatter` on a ROS 2 machine in the same domain receives
// these messages (rmw_cyclonedds).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "rcllite/rcllite.hpp"
#include "rcllite_types/rcl_interfaces/parameter.hpp"

int main(int argc, char** argv) {
  const size_t max_count = argc > 1 ? static_cast<size_t>(std::atoi(argv[1])) : 0;
  auto node = std::make_shared<rcl::Node>("talker");
  auto pub = node->create_publisher<rcl_interfaces::msg::Parameter>("chatter");

  size_t count = 0;
  while (max_count == 0 || count < max_count) {
    rcl_interfaces::msg::Parameter msg;
    msg.name = "Hello, world! " + std::to_string(count);
    pub->publish(msg);
    std::fflush(stdout);
    std::printf("publishing: '%s'\n", msg.name.c_str());
    ++count;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  return 0;
}
