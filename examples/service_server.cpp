// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Service server demo using the official rcl_interfaces/GetParameters type:
// answers every requested parameter name with its name hashed into an
// integer, so `ros2 service call` from a ROS 2 machine (rmw_cyclonedds)
// gets a deterministic response.
#include <cstdio>

#include "rcllite/rcllite.hpp"
#include "rcllite_types/rcl_interfaces/get_parameters.hpp"

int main() {
  auto node = std::make_shared<rcl::Node>("get_parameters_server");
  node->create_service<rcl_interfaces::srv::GetParameters>(
      "get_parameters", [](const rcl_interfaces::srv::GetParameters::Request& req) {
        rcl_interfaces::srv::GetParameters::Response resp;
        for (const auto& name : req.names) {
          rcl_interfaces::msg::ParameterValue v;
          v.type = 2;  // PARAMETER_INTEGER
          v.integer_value =
              static_cast<int64_t>(std::hash<std::string>{}(name) & 0xFFFF);
          resp.values.push_back(v);
        }
        return resp;
      });
  std::printf(
      "ready: ros2 service call /get_parameters rcl_interfaces/srv/GetParameters "
      "\"{names: [hello]}\"\n");
  std::fflush(stdout);

  rcl::spin(node);
  return 0;
}
