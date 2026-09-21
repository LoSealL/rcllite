// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Service client demo using the official rcl_interfaces/GetParameters type;
// interoperates with the ROS 2 demo_nodes server of the same type
// (rmw_cyclonedds).
#include <chrono>
#include <cstdio>

#include "rcllite/rcllite.hpp"
#include "rcllite_types/rcl_interfaces/get_parameters.hpp"

int main() {
  auto node = std::make_shared<rcl::Node>("get_parameters_client");
  auto client =
      node->create_client<rcl_interfaces::srv::GetParameters>("get_parameters");
  if (!client->wait_for_service(std::chrono::seconds(10))) {
    std::printf("service not available\n");
    return 1;
  }

  // A spinning executor on another thread is required for the synchronous
  // call below to receive its response.
  rcl::Executor exec;
  exec.add_node(node);
  std::thread spinner([&exec]() { exec.spin(); });

  rcl_interfaces::srv::GetParameters::Request req;
  req.names = {"speed", "name"};
  auto resp = client->call(req, std::chrono::seconds(10));
  for (size_t i = 0; i < req.names.size(); ++i) {
    std::printf("%s -> type=%d integer=%lld string='%s'\n", req.names[i].c_str(),
                resp.values[i].type,
                static_cast<long long>(resp.values[i].integer_value),
                resp.values[i].string_value.c_str());
  }
  std::fflush(stdout);

  exec.cancel();
  spinner.join();
  return 0;
}
