// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#ifndef RCLLITE__GRAPH_HPP_
#define RCLLITE__GRAPH_HPP_

#include <memory>
#include <string>

namespace rcl {

/// Passive runtime-graph observer ("graph sniffer").
///
/// Owns a dedicated anonymous DDS participant and merges three sources:
///
///   * the DCPSParticipant / DCPSPublication / DCPSSubscription built-in
///     topics — the ground truth of who is live on the domain; endpoints
///     carry participant GUID, topic and type names.  This also sees
///     rcllite's own endpoints (plain DDS, no graph protocol needed).
///   * the rmw_dds_common "ros_discovery_info" topic — authoritative node
///     names and endpoint ownership for ROS 2 peers.
///   * "rt/rosout" — rcl_interfaces/Log records published by ROS 2 nodes.
///
/// Endpoints are attributed to nodes via ros_discovery_info first (exact
/// endpoint-GID match), then via the participant USER_DATA enclave that
/// rcllite itself sets.  The monitor's own endpoints are excluded.
///
/// A background thread polls a waitset; snapshot_json() can be called from
/// any thread at any time.
class GraphMonitor {
 public:
  /// Creates the participant, readers and the poll thread.
  GraphMonitor();

  /// Stops the poll thread and tears the participant down.
  ~GraphMonitor();

  GraphMonitor(const GraphMonitor&) = delete;
  GraphMonitor& operator=(const GraphMonitor&) = delete;

  /// One JSON document describing the current graph:
  ///   nodes / topics / services / edges / endpoints / logs.
  /// Edges are directed data-flow links (publisher node -> subscription
  /// node, service client -> service server); plumbing topics
  /// (ros_discovery_info, rt/rosout, rt/parameter_events, DCPS*) are kept
  /// in the raw data but excluded from edges.  "logs" is a bounded ring of
  /// the most recent rt/rosout records.
  std::string snapshot_json();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rcl

#endif  // RCLLITE__GRAPH_HPP_
