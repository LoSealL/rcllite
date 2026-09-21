// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#ifndef RCLLITE__QOS_HPP_
#define RCLLITE__QOS_HPP_

#include <cstddef>

namespace rcl {

/// The subset of rmw_qos_profile_t that is meaningful for rcllite.
/// Defaults mirror rmw_qos_profile_default (reliable / volatile / keep-last 10).
struct QoS {
  enum class Reliability : uint8_t { BestEffort, Reliable };
  enum class Durability : uint8_t { Volatile, TransientLocal };
  enum class History : uint8_t { KeepLast, KeepAll };

  Reliability reliability = Reliability::Reliable;
  Durability durability = Durability::Volatile;
  History history = History::KeepLast;
  size_t depth = 10;

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

#endif  // RCLLITE__QOS_HPP_
