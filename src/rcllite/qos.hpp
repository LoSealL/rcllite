// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#ifndef RCLLITE_QOS_HPP_
#define RCLLITE_QOS_HPP_

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace rcl {

/// The subset of rmw_qos_profile_t that is meaningful for rcllite.
/// Defaults mirror rmw_qos_profile_default (reliable / volatile / keep-last 10).
struct QoS {
  enum class Reliability : uint8_t { BestEffort, Reliable };
  enum class Durability : uint8_t { Volatile, TransientLocal };
  enum class History : uint8_t { KeepLast, KeepAll };
  /// Publisher-side flow control (not a DDS wire property): when the in-flight
  /// window is full, block until the matched readers acknowledge instead of
  /// overwriting the oldest sample.  Writer-only; requires Reliable + KeepLast.
  enum class Wait : uint8_t { None, Forever, Timeout };

  Reliability reliability = Reliability::Reliable;
  Durability durability = Durability::Volatile;
  History history = History::KeepLast;
  size_t depth = 10;
  /// Reader-side cap (0 = unlimited): with a cap the reader runs keep-all +
  /// resource limits and rejects samples once full.  A reliable publisher's
  /// ack wait then stalls instead of the reader silently overwriting.
  size_t max_samples = 0;
  Wait wait = Wait::None;
  std::chrono::nanoseconds wait_timeout{0};

  QoS& set_best_effort() {
    reliability = Reliability::BestEffort;
    return *this;
  }
  QoS& set_transient_local() {
    durability = Durability::TransientLocal;
    return *this;
  }
  QoS& keep_last(size_t d) {
    history = History::KeepLast;
    depth = d;
    return *this;
  }
  /// Reader-side bounded box: keep at most `n` samples and refuse new ones
  /// once full, so a paired `wait_for()` publisher stalls instead of data
  /// being overwritten.  Continuous streams only: one-shot latches (done,
  /// calibration, model paths) must stay keep-last for late joiners.  Pair
  /// with `wait_for`, never `wait_forever`, and expect a stalled consumer to
  /// hold up the whole DDS connection, not just this topic.
  QoS& refuse_when_full(size_t n) {
    // KeepAll is what makes the rhc enforce max_samples instead of
    // overwriting (depth only exists for KeepLast).
    history = History::KeepAll;
    max_samples = n;
    return *this;
  }
  /// Block when the in-flight window is full, without a deadline.
  QoS& wait_forever() {
    wait = Wait::Forever;
    return *this;
  }
  /// Block when the in-flight window is full, at most `timeout` per publish.
  QoS& wait_for(std::chrono::nanoseconds timeout) {
    wait = Wait::Timeout;
    wait_timeout = timeout;
    return *this;
  }

  /// rmw_qos_profile_default
  static QoS Default() { return QoS(); }
  /// rmw_qos_profile_sensor_data
  static QoS SensorData() { return QoS().set_best_effort().keep_last(5); }
  /// rmw_qos_profile_parameters (parameter services)
  static QoS Parameters() { return QoS().keep_last(10); }
  /// rmw_qos_profile_parameter_events
  static QoS ParameterEvents() { return QoS().set_transient_local().keep_last(1000); }
  /// rmw_qos_profile_services_default
  static QoS Services() { return QoS().keep_last(10); }
};

}  // namespace rcl

#endif  // RCLLITE_QOS_HPP_
