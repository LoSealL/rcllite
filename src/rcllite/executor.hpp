// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#ifndef RCLLITE__EXECUTOR_HPP_
#define RCLLITE__EXECUTOR_HPP_

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "dds/dds.h"
#include "rcllite/dds/entities.hpp"
#include "rcllite/node.hpp"

namespace rcl {

/// Single-threaded executor modelled after rclcpp's, reduced to the
/// essentials: wait on all attached conditions (read conditions of
/// subscriptions/services/clients plus node change guards plus the cancel
/// guard), then dispatch whatever became ready.
///
/// Nodes may be added/removed and entities created/destroyed while spinning;
/// node change guards wake the executor and the attachment set is reconciled.
class Executor {
 public:
  Executor();
  ~Executor();

  Executor(const Executor&) = delete;
  Executor& operator=(const Executor&) = delete;

  void add_node(std::shared_ptr<Node> node);
  void remove_node(const std::shared_ptr<Node>& node);

  /// Process events until cancel().  Callable once at a time.
  void spin();

  /// Wait up to `timeout` for one batch of events and process them.
  /// A negative timeout means wait forever.  Returns true if any events
  /// were processed.
  bool spin_once(std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));

  /// Process everything currently ready without waiting.
  /// Returns true if any work was done.
  bool spin_some();

  /// Stop a running spin() from any thread.
  void cancel();

 private:
  struct Attached {
    dds_entity_t cond = DDS_RETCODE_ERROR;  // attached condition handle
    std::shared_ptr<Node> node;             // null for internal guards
    std::shared_ptr<EntityBase> entity;     // null for guards
  };

  /// Rebuild the waitset attachment set from the current node entities.
  /// Callers must hold nodes_mtx_.
  void reconcile_locked();

  bool wait_and_process(dds_duration_t timeout);

  dds_entity_t waitset_ = DDS_RETCODE_ERROR;
  dds::GuardCondition cancel_guard_;
  dds::GuardCondition wake_guard_;  // add_node wakeups, no cancel semantics
  std::atomic<bool> cancel_requested_{false};

  std::mutex nodes_mtx_;
  std::vector<std::shared_ptr<Node>> nodes_;

  // attachment bookkeeping (guarded by nodes_mtx_): attachment id -> entry
  std::unordered_map<dds_attach_t, Attached> attached_;
  std::unordered_map<const Node*, uint64_t /*epoch*/> node_epochs_;
  uint64_t next_attach_id_ = 2;  // 1/2 are reserved for the executor guards
};

/// Convenience: spin one node until cancelled.
void spin(std::shared_ptr<Node> node);

}  // namespace rcl

#endif  // RCLLITE__EXECUTOR_HPP_
