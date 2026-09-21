// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Byte-level checks of the CDR engine against known-good ROS 2 encodings plus
// writer/reader roundtrips for the generated official interface types.
#include "rcllite/cdr.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "rcllite/service.hpp"
#include "rcllite_types/builtin_interfaces/time.hpp"
#include "rcllite_types/rcl_interfaces/get_parameters.hpp"
#include "rcllite_types/rcl_interfaces/parameter.hpp"
#include "rcllite_types/rosgraph_msgs/clock.hpp"

namespace {

using rcl::CdrReader;
using rcl::CdrWriter;

void expect_bytes(const std::vector<uint8_t>& actual, std::vector<uint8_t> expected) {
  ASSERT_EQ(actual.size(), expected.size()) << "payload length mismatch";
  for (size_t i = 0; i < expected.size(); ++i) {
    ASSERT_EQ(actual[i], expected[i]) << "byte " << i << " differs";
  }
}

TEST(Cdr, EncapsulationAndFixedStruct) {
  builtin_interfaces::msg::Time t;
  t.sec = 5;
  t.nanosec = 6;
  CdrWriter w;
  builtin_interfaces::msg::Time::serialize(t, w);
  // 00 01 00 00 | sec(4) | nanosec(4)
  expect_bytes(w.payload(), {0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x06, 0x00,
                             0x00, 0x00});

  builtin_interfaces::msg::Time back;
  CdrReader r(w.payload());
  ASSERT_TRUE(builtin_interfaces::msg::Time::deserialize(back, r));
  EXPECT_EQ(back.sec, 5);
  EXPECT_EQ(back.nanosec, 6u);
}

TEST(Cdr, StringEncoding) {
  // ROS 2 `string`: uint32 length (incl. NUL) + bytes + NUL.
  CdrWriter w;
  w.write_string("Hi");
  expect_bytes(w.payload(),
               {0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x48, 0x69, 0x00});

  CdrReader r(w.payload());
  EXPECT_EQ(r.read_string(), "Hi");
}

TEST(Cdr, EightByteAlignment) {
  // uint32 followed by uint64 forces 4 bytes of alignment padding.
  CdrWriter w;
  w.write_uint32(1);
  w.write_uint64(2);
  expect_bytes(w.payload(),
               {0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});

  // Service header (uint64 + int64) keeps every field 8-aligned.
  rcl::ServiceHeader hdr;
  hdr.client_id = 0x1122334455667788ull;
  hdr.sequence = 7;
  CdrWriter sw;
  rcl::write_service_header(sw, hdr);
  expect_bytes(sw.payload(),
               {0x00, 0x01, 0x00, 0x00, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33,
                0x22, 0x11, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  CdrReader sr(sw.payload());
  const rcl::ServiceHeader hdr2 = rcl::read_service_header(sr);
  EXPECT_EQ(hdr2.client_id, 0x1122334455667788ull);
  EXPECT_EQ(hdr2.sequence, 7);
}

TEST(Cdr, NestedStructRoundtrip) {
  rosgraph_msgs::msg::Clock c;
  c.clock.sec = 1726000000;
  c.clock.nanosec = 42;
  CdrWriter w;
  rosgraph_msgs::msg::Clock::serialize(c, w);
  rosgraph_msgs::msg::Clock back;
  CdrReader r(w.payload());
  ASSERT_TRUE(rosgraph_msgs::msg::Clock::deserialize(back, r));
  EXPECT_EQ(back.clock.sec, 1726000000);
  EXPECT_EQ(back.clock.nanosec, 42u);
}

TEST(Cdr, ParameterRoundtrip) {
  // Parameter = string + nested ParameterValue (variant + six sequences):
  // covers strings, nested structs and sequence alignment in one type.
  rcl_interfaces::msg::Parameter p;
  p.name = "answer";
  p.value.type = 2;  // PARAMETER_INTEGER
  p.value.integer_value = -1;
  p.value.string_value = "x";
  p.value.byte_array_value = {1, 2, 3};
  p.value.string_array_value = {"a", "bc"};

  CdrWriter w;
  rcl_interfaces::msg::Parameter::serialize(p, w);
  rcl_interfaces::msg::Parameter back;
  CdrReader r(w.payload());
  ASSERT_TRUE(rcl_interfaces::msg::Parameter::deserialize(back, r));
  EXPECT_EQ(back.name, "answer");
  EXPECT_EQ(back.value.type, 2);
  EXPECT_EQ(back.value.integer_value, -1);
  EXPECT_EQ(back.value.string_value, "x");
  EXPECT_EQ(back.value.byte_array_value.size(), 3u);
  EXPECT_EQ(back.value.byte_array_value[2], 3);
  EXPECT_EQ(back.value.string_array_value.size(), 2u);
  EXPECT_EQ(back.value.string_array_value[1], "bc");
}

TEST(Cdr, ServiceRequestRoundtrip) {
  // GetParameters::Request is a sequence<string>: its length prefix is
  // 4-aligned right after the 16-byte service header.
  rcl::CdrWriter w;
  rcl::ServiceHeader hdr{0xABCD, 9};
  rcl::write_service_header(w, hdr);
  rcl_interfaces::srv::GetParameters::Request req;
  req.names = {"speed", "name"};
  rcl_interfaces::srv::GetParameters::Request::serialize(req, w);

  rcl::CdrReader r(w.payload());
  const rcl::ServiceHeader h2 = rcl::read_service_header(r);
  EXPECT_EQ(h2.client_id, uint64_t{0xABCD});
  EXPECT_EQ(h2.sequence, 9);
  rcl_interfaces::srv::GetParameters::Request back;
  ASSERT_TRUE(rcl_interfaces::srv::GetParameters::Request::deserialize(back, r));
  ASSERT_EQ(back.names.size(), 2u);
  EXPECT_EQ(back.names[0], "speed");
  EXPECT_EQ(back.names[1], "name");
}

TEST(Cdr, StringSequenceHelpers) {
  CdrWriter w;
  std::vector<std::string> vs{"a", "bc"};
  rcl::write_string_sequence(w, vs);
  CdrReader r(w.payload());
  std::vector<std::string> out;
  ASSERT_TRUE(rcl::read_string_sequence(r, out));
  EXPECT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0], "a");
  EXPECT_EQ(out[1], "bc");
}

}  // namespace
