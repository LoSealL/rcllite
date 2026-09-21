// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Minimal subscriber demo using the official rcl_interfaces/Parameter type;
// interoperates with `ros2 topic pub` (rmw_cyclonedds).
#include <cstdio>

#include "rcllite/rcllite.hpp"
#include "rcllite_types/rcl_interfaces/parameter.hpp"

int main() {
  auto node = std::make_shared<rcl::Node>("listener");
  node->create_subscription<rcl_interfaces::msg::Parameter>(
      "chatter", [](const rcl_interfaces::msg::Parameter& m) {
        std::printf("I heard: [%s]\n", m.name.c_str());
        std::fflush(stdout);
      });

  rcl::spin(node);
  return 0;
}
