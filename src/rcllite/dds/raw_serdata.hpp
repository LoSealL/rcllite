// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Opaque-CDR sertype/serdata integration for CycloneDDS 0.10.x.
//
// ROS 2 wire compatibility only requires that the DDS topic carries the right
// type *name* and a correctly serialized CDR payload.  We therefore hook a
// custom (ser)type into CycloneDDS that treats every sample as an opaque CDR
// blob: rcllite serializes messages itself with CdrWriter and injects the
// result via dds_writecdr(); incoming samples arrive as raw CDR blobs via
// dds_takecdr().  This is the same approach rmw_cyclonedds uses for
// rmw_publish_serialized_message, minus the introspection machinery.
#ifndef RCLLITE__DDS__RAW_SERDATA_HPP_
#define RCLLITE__DDS__RAW_SERDATA_HPP_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "dds/dds.h"
#include "dds/ddsi/ddsi_serdata.h"
#include "dds/ddsi/ddsi_sertype.h"

namespace rcl {
namespace dds {

/// A serdata whose payload is an opaque CDR blob (including the 4-byte
/// encapsulation header).  The payload is padded to a multiple of 4 because
/// CycloneDDS' network path reads 4-byte chunks.
struct RawSerdata {
  struct ddsi_serdata c;
  std::vector<uint8_t> payload;
};

/// Create an owned sertype with the given DDS type name (e.g.
/// "std_msgs::msg::dds_::String_").  Ownership passes to the topic created
/// from it via dds_create_topic_sertype(); do not free it manually.
struct ddsi_sertype* create_raw_sertype(const char* type_name);

/// Wrap an already serialized CDR payload (encapsulation header included)
/// into a serdata usable with dds_writecdr().  The payload is copied.
struct ddsi_serdata* raw_serdata_from_blob(const struct ddsi_sertype* type,
                                           const void* blob, size_t size);

/// Access the opaque payload of a serdata returned by dds_takecdr().
/// The blob includes the 4-byte encapsulation header.
const uint8_t* raw_serdata_payload(const struct ddsi_serdata* d);
size_t raw_serdata_payload_size(const struct ddsi_serdata* d);

}  // namespace dds
}  // namespace rcl

#endif  // RCLLITE__DDS__RAW_SERDATA_HPP_
