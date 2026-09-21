// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Declares parameters, then reflects every change — including remote ones
// made with `ros2 param set` (rmw_cyclonedds) — on /parameter_events.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "rcllite/rcllite.hpp"

int main(int argc, char** argv) {
  const int seconds = argc > 1 ? std::atoi(argv[1]) : 10;
  auto node = std::make_shared<rcl::Node>("parameters_demo");

  node->declare_parameter("my_int", 42);
  node->declare_parameter("my_string", std::string("hello"));
  node->declare_parameter("my_double", 3.14);

  rcl::Executor executor;
  executor.add_node(node);
  std::thread spinner([&executor]() { executor.spin(); });

  for (int i = 0; i < seconds; ++i) {
    std::printf("my_int=%ld my_string='%s' my_double=%.2f\n",
                static_cast<long>(node->get_parameter<int64_t>("my_int")),
                node->get_parameter<std::string>("my_string").c_str(),
                node->get_parameter<double>("my_double"));
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(1));

    node->set_parameter("my_int",
                        rcl::param::to_value(static_cast<int64_t>(42 + i + 1)));
  }

  executor.cancel();
  spinner.join();
  return 0;
}
