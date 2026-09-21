// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Thin RAII wrappers around the CycloneDDS C entities rcllite needs.
// Every writer/reader talks "opaque CDR": payloads are pre-serialized blobs.
#ifndef RCLLITE__DDS__ENTITIES_HPP_
#define RCLLITE__DDS__ENTITIES_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "dds/dds.h"
#include "rcllite/dds/raw_serdata.hpp"
#include "rcllite/exception.hpp"
#include "rcllite/qos.hpp"

namespace rcl {
namespace dds {

/// Process-wide context: resolves ROS_DOMAIN_ID / ROS_LOCALHOST_ONLY once and
/// keeps CycloneDDS initialized until shutdown().
class Context {
 public:
  static Context& instance();
  static void init() { instance(); }
  static void shutdown();

  uint32_t domain_id() const { return domain_id_; }
  bool localhost_only() const { return localhost_only_; }

 private:
  Context();
  ~Context() = default;
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  uint32_t domain_id_ = 0;
  bool localhost_only_ = false;
};

/// Convert rcllite QoS into a dds_qos_t (caller deletes with dds_delete_qos).
dds_qos_t* make_dds_qos(const QoS& qos);

/// A DDS participant carrying the node identity in its USER_DATA, exactly
/// like ROS 2 does ("enclave=/ns/node;") so `ros2 node list` can see it.
class Participant {
 public:
  Participant(uint32_t domain_id, const std::string& enclave);
  ~Participant();

  Participant(const Participant&) = delete;
  Participant& operator=(const Participant&) = delete;

  dds_entity_t handle() const { return pp_; }
  dds_entity_t publisher() const { return pub_; }
  dds_entity_t subscriber() const { return sub_; }

 private:
  dds_entity_t pp_ = DDS_RETCODE_ERROR;
  dds_entity_t pub_ = DDS_RETCODE_ERROR;
  dds_entity_t sub_ = DDS_RETCODE_ERROR;
};

class Writer {
 public:
  Writer(dds_entity_t participant, dds_entity_t dds_publisher, const std::string& topic,
         const std::string& type_name, const QoS& qos);
  ~Writer();

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  /// Publish a serialized CDR payload (encapsulation header included).
  /// Thread-safe: CycloneDDS writers may be written from any thread.
  bool write(const uint8_t* payload, size_t size);

 private:
  dds_entity_t wr_ = DDS_RETCODE_ERROR;
  struct ddsi_sertype* sertype_ = nullptr;  // owned by the topic
};

class Reader {
 public:
  Reader(dds_entity_t participant, dds_entity_t dds_subscriber,
         const std::string& topic, const std::string& type_name, const QoS& qos);
  ~Reader();

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  /// Take all currently available samples, invoking
  /// fn(payload incl. encapsulation header, size, sample_info) for each.
  /// Returns the number of samples taken.  Throws on transport errors.
  template <typename F>
  size_t take(F&& fn);

  /// Condition to attach to a waitset; triggers when data is available.
  dds_entity_t read_condition() const { return rc_; }

  /// Number of matched writers (service availability check).
  size_t writer_count() const;

 private:
  dds_entity_t rd_ = DDS_RETCODE_ERROR;
  dds_entity_t rc_ = DDS_RETCODE_ERROR;  // read condition
};

template <typename F>
size_t Reader::take(F&& fn) {
  // Batch of 16: amortizes the per-take call overhead over bursts without
  // inflating the stack footprint for quiet readers.
  struct ddsi_serdata* serdatas[16];
  dds_sample_info_t infos[16];
  size_t taken_total = 0;
  for (;;) {
    const dds_return_t n = dds_takecdr(rd_, serdatas, 16, infos, DDS_ANY_STATE);
    if (n < 0) {
      throw Error(vila::InternalError("dds_takecdr failed: {}", n));
    }
    for (dds_return_t i = 0; i < n; ++i) {
      if (infos[i].valid_data) {
        fn(raw_serdata_payload(serdatas[i]), raw_serdata_payload_size(serdatas[i]),
           infos[i]);
      }
      ddsi_serdata_unref(serdatas[i]);
    }
    taken_total += static_cast<size_t>(n > 0 ? n : 0);
    if (n < 16) {
      break;
    }
  }
  return taken_total;
}

/// Guard condition usable from any thread to wake a waitset/executor.
class GuardCondition {
 public:
  GuardCondition();
  ~GuardCondition();

  GuardCondition(const GuardCondition&) = delete;
  GuardCondition& operator=(const GuardCondition&) = delete;

  void trigger();
  bool take();
  dds_entity_t handle() const { return gcond_; }

 private:
  dds_entity_t gcond_ = DDS_RETCODE_ERROR;
};

/// Serialize QoS values into a CycloneDDS config fragment honoring
/// ROS_LOCALHOST_ONLY (mirrors rcl's default localhost-only on macOS/Windows).
std::string default_config_xml(bool localhost_only);

}  // namespace dds
}  // namespace rcl

#endif  // RCLLITE__DDS__ENTITIES_HPP_
