// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Test-side control API for the mock ddsc library (see mock_ddsc.cc).
// The mock replaces the CycloneDDS C API at link time so unit tests can
// drive rcllite deterministically without any network activity:
//
//   * samples "arrive" via enqueue_sample() and are drained by
//     Reader::take -> dds_takecdr during Executor dispatch;
//   * written payloads are recorded per writer and inspectable;
//   * waitsets report exactly what the tests queued / triggered;
//   * QoS translation is captured at entity creation for assertions.
#ifndef TESTS_MOCK_MOCK_DDSC_H_
#define TESTS_MOCK_MOCK_DDSC_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "dds/dds.h"

namespace mock_dds {

/// Clear all state (entities, queues, records, configured results).
/// Call at the start of every test, before any rcllite object is created.
void reset();

/// Handles of the most recently created entities (creation order inside
/// the rcllite constructors is stable, which the tests rely on).
dds_entity_t last_reader();
dds_entity_t last_writer();
dds_entity_t last_readcondition();

/// Deliver a serialized sample "from the network" on a reader; it becomes
/// visible to the next dds_takecdr / executor dispatch.
void enqueue_sample(dds_entity_t reader, const void* data, size_t size);

/// Payloads (CDR blobs incl. encapsulation header) written so far.
const std::vector<std::vector<uint8_t>>& written(dds_entity_t writer);

/// Make subsequent dds_writecdr calls return this code (DDS_RETCODE_OK
/// restores normal behaviour).
void set_write_result(dds_return_t rc);

/// Number of matched writers a reader reports (drives wait_for_service).
void set_matched_writers(dds_entity_t reader, int32_t count);

/// Number of matched readers a writer reports (drives wait_for_subscribers
/// and the backpressure publish overload).
void set_matched_readers(dds_entity_t writer, int32_t count);

/// QoS values captured when a reader/writer was created (as translated by
/// rcl::dds::make_dds_qos).
struct QosView {
  dds_history_kind_t history;
  int32_t depth;
  dds_reliability_kind_t reliability;
  dds_durability_kind_t durability;
};

const QosView* writer_qos(dds_entity_t writer);
const QosView* reader_qos(dds_entity_t reader);

}  // namespace mock_dds

#endif  // TESTS_MOCK_MOCK_DDSC_H_
