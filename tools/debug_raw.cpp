// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
// Debug tool: subscribe an arbitrary raw DDS topic and dump CDR payloads.
//   debug_raw <topic> <dds-type-name> [seconds]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "rcllite/dds/entities.hpp"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: %s <topic> <dds-type> [seconds]\n", argv[0]);
    return 1;
  }
  const int seconds = argc > 3 ? std::atoi(argv[3]) : 10;
  rcl::dds::Context::init();
  rcl::dds::Participant pp(0, "/debug_raw");
  rcl::dds::Reader reader(pp.handle(), pp.subscriber(), argv[1], argv[2],
                          rcl::QoS().set_transient_local().keep_last(10));

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  size_t count = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    reader.take([&count](const uint8_t* p, size_t n, const dds_sample_info_t&) {
      std::printf("sample %zu (%zu bytes):", count++, n);
      for (size_t i = 0; i < n && i < 160; ++i) {
        std::printf(" %02x", p[i]);
      }
      std::printf("\n");
      std::fflush(stdout);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  std::printf("total %zu samples\n", count);
  return 0;
}
