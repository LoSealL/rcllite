// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#ifndef RCLLITE__GRAPH_EXECUTOR_HPP_
#define RCLLITE__GRAPH_EXECUTOR_HPP_

#include <chrono>
#include <memory>
#include <string>

namespace rcl {

/// Config-driven topology launcher: instantiates every node described by a
/// topology document and runs them on one shared Executor — equivalent to
/// each node executing its own configuration separately, co-located in a
/// single process (communication still goes through DDS loopback; rcllite
/// has no intra-process path).
///
/// The document is JSON (the Python layer accepts the same content as YAML
/// and converts).  Schema:
///
/// {
///   "nodes": [
///     {
///       "name": "talker",                  // required
///       "namespace": "/",                  // optional, default "/"
///       "class": "my::MyTalker",           // optional: a node subclass
///                                          // registered with
///                                          // RCLLITE_REGISTER_NODE; its
///                                          // constructor's behavior runs,
///                                          // then the config below is
///                                          // assembled on top.  Unknown
///                                          // class names are an error.
///       "parameters": {"rate": 10},        // optional; bool/int/double/string
///       "publishers": [                    // optional
///         {
///           "topic": "/chatter",           // required; relative names are
///                                          // expanded against the namespace
///           "type": "rcl_interfaces::msg::dds_::Parameter_",
///                                          // "pkg/pkg/Name" is accepted and
///                                          // rewritten to the DDS name
///           "payload_hex": "00010000...",  // optional; raw CDR incl. the
///                                          // encapsulation header; defaults
///                                          // to a bare CDR-LE header
///           "rate_hz": 1.0,                // optional; absent/0 -> publish
///                                          // once on start
///           "qos": {"reliability": "reliable"|"best_effort",
///                   "durability": "volatile"|"transient_local",
///                   "depth": 10}           // all optional
///         }
///       ],
///       "subscriptions": [                 // optional
///         {"topic": "/chatter", "type": "...", "qos": {...}}
///       ]
///     }
///   ]
/// }
///
/// Subscriptions are raw: samples are counted (see status_json) but not
/// deserialized — the graph carries structure, not behavior.  Services are
/// not part of the schema yet.  Periodic publishers use one thread each.
class GraphExecutor {
 public:
  /// Parses and validates the whole topology, creating every node and
  /// endpoint; throws rcl::Error on malformed input.
  explicit GraphExecutor(const std::string& json_topology);

  /// Stops publisher threads and tears every node down.
  ~GraphExecutor();

  GraphExecutor(const GraphExecutor&) = delete;
  GraphExecutor& operator=(const GraphExecutor&) = delete;

  /// Blocking spin of all nodes until cancel() (from any thread).
  void spin();

  /// One wait-and-dispatch round across all nodes.  A negative timeout
  /// waits forever.  Returns true if any events were processed.
  bool spin_once(std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));

  /// Stop a running spin() from any thread.
  void cancel();

  /// Per-node/per-endpoint counters as JSON: published / received.
  std::string status_json() const;

  /// Names of all RCLLITE_REGISTER_NODE classes in this binary, as a JSON
  /// array (sorted).  These are the values a topology "class" field accepts.
  static std::string registered_nodes_json();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rcl

#endif  // RCLLITE__GRAPH_EXECUTOR_HPP_
