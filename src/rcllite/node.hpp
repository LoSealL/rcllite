// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#ifndef RCLLITE_NODE_HPP_
#define RCLLITE_NODE_HPP_

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "rcllite/dds/entities.hpp"
#include "rcllite/parameter.hpp"
#include "rcllite/qos.hpp"

namespace rcl {

class Node;
class EntityBase;
class ParameterCore;
template <typename MessageT>
class Publisher;
template <typename MessageT>
class Subscription;
template <typename ServiceT>
class Service;
template <typename ServiceT>
class Client;

/// Anything an Executor can wait on: exposes a DDS condition handle and a
/// dispatch callback that drains pending work.
class EntityBase {
 public:
  virtual ~EntityBase() = default;
  virtual dds_entity_t condition() const = 0;
  virtual void dispatch() = 0;
};

/// Base for the template-free parts of Node shared with parameter support.
class Node : public std::enable_shared_from_this<Node> {
 public:
  explicit Node(const std::string& name);
  Node(const std::string& name, const std::string& ns);
  ~Node();

  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  const std::string& get_name() const { return name_; }
  const std::string& get_namespace() const { return ns_; }
  std::string get_fully_qualified_name() const { return fq_name_; }

  dds::Participant& participant() { return *ppant_; }
  uint32_t domain_id() const { return domain_id_; }

  template <typename MessageT>
  std::shared_ptr<class Publisher<MessageT>> create_publisher(
      const std::string& topic, const QoS& qos = QoS::Default());

  template <typename MessageT, typename CallbackT>
  std::shared_ptr<class Subscription<MessageT>> create_subscription(
      const std::string& topic, CallbackT&& callback, const QoS& qos = QoS::Default());

  template <typename ServiceT>
  std::shared_ptr<class Service<ServiceT>> create_service(
      const std::string& service_name, typename Service<ServiceT>::Callback callback,
      const QoS& qos = QoS::Services());

  template <typename ServiceT>
  std::shared_ptr<Client<ServiceT>> create_client(const std::string& service_name,
                                                  const QoS& qos = QoS::Services());

  /// Publish ParticipantEntitiesInfo on ros_discovery_info so ROS 2 CLI
  /// tools see this node (rmw_dds_common graph protocol).
  void announce_to_ros_graph();

  // --- parameters -----------------------------------------------------------

  template <typename T>
  void declare_parameter(const std::string& name, const T& default_value);
  void declare_parameter(const std::string& name);
  param::ParameterValue get_parameter(const std::string& name) const;
  template <typename T>
  T get_parameter_or(const std::string& name, const T& alternative) const;
  template <typename T>
  T get_parameter(const std::string& name) const;
  bool set_parameter(const std::string& name, const param::ParameterValue& value);

  // --- executor plumbing ---------------------------------------------------

  /// Bumped every time the set of waitable entities changes; an executor
  /// re-attaches when it observes a change.
  uint64_t entity_epoch() const;

  /// Triggered on entity set changes so executors wake up and re-attach.
  dds::GuardCondition& change_guard();

  /// Snapshot of the current waitable entities.
  std::vector<std::shared_ptr<EntityBase>> entities() const;

  /// Register a waitable entity.  Called by create_* implementations.
  void add_entity(std::shared_ptr<EntityBase> entity);

  /// Unregister a waitable entity (bumps the epoch so executors re-attach;
  /// the entity object itself survives until the last holder drops it, so an
  /// executor mid-dispatch stays safe).
  void remove_entity(const std::shared_ptr<EntityBase>& entity);

  /// Access (lazily creating) the parameter support: the six parameter
  /// services plus the /parameter_events publisher.
  ParameterCore& params() const;

  // --- endpoint topology -----------------------------------------------------

  /// Expanded topic names this node publishes on, in creation order and
  /// including dual-QoS duplicates.  A host reads this after construction
  /// quiesces to derive endpoint topology without discovery.
  std::vector<std::string> pub_topics() const;

  /// Expanded topic names this node subscribes to, same contract as
  /// pub_topics().
  std::vector<std::string> sub_topics() const;

  // --- lifecycle -------------------------------------------------------------

  /// Wire endpoints (create publishers/subscriptions) here: the owning
  /// GraphExecutor calls it once for every node, after all nodes are
  /// constructed and before anything streams.  Final-output publishing
  /// belongs in on_deinit(), never here.
  virtual void on_init() {}

  /// Finalize here: write artifacts, publish the final latches, and return
  /// only once every output is out.  The executor calls it in topological
  /// order -- after every feeder's on_deinit returned -- so subscriptions
  /// already hold the feeders' finals.
  virtual void on_deinit() {}

  /// Lifecycle event sink installed by the owning host (GraphExecutor, a
  /// standalone wrapper) before on_init() runs.  The seam for future
  /// cross-process coordination: a remote-backed host can feed the same
  /// events from off-process nodes.
  struct LifecycleSink {
    /// mark_batch_finished() lands here (the executor may then run
    /// on_deinit once the feeders are done).
    std::function<void(Node&)> batch_finished;
    /// request_graph_shutdown() lands here (window closed, fatal error).
    std::function<void()> shutdown_requested;
    /// Park a streaming worker until the whole graph wired (every on_init
    /// ran); false = the graph is unwinding, exit without streaming.
    std::function<bool()> wait_graph_active;
  };

  /// Install the lifecycle sink; must precede on_init().
  void set_lifecycle_sink(LifecycleSink sink);

  /// Report this node's batch complete.  Idempotence is the host's call
  /// (the executor ignores repeats).  Without a sink: a logged no-op, so
  /// unhosted nodes never hang anything.
  void mark_batch_finished();

  /// Ask the host to stop the whole graph (window closed, fatal error).
  /// Without a sink: a no-op.
  void request_graph_shutdown();

  /// Park until every on_init ran (see LifecycleSink).  True when active
  /// or no sink is installed (standalone nodes stream immediately).
  bool wait_graph_active();

 private:
  friend class ParameterCore;

  /// Endpoint-topology bookkeeping, guarded by entities_mtx_.
  void note_pub_topic(const std::string& fqn);
  void note_sub_topic(const std::string& fqn);

  std::string name_;
  std::string ns_;
  std::string fq_name_;
  uint32_t domain_id_;
  std::unique_ptr<dds::Participant> ppant_;

  mutable std::mutex entities_mtx_;
  std::vector<std::shared_ptr<EntityBase>> entities_;
  std::vector<std::string> pub_topics_;  ///< Endpoint topology (see accessor).
  std::vector<std::string> sub_topics_;  ///< Endpoint topology (see accessor).
  uint64_t entity_epoch_ = 0;
  std::unique_ptr<dds::GuardCondition> change_guard_;

  mutable std::mutex lifecycle_mtx_;
  LifecycleSink lifecycle_sink_;  ///< Host-installed; see set_lifecycle_sink().

  mutable std::mutex params_mtx_;
  mutable std::unique_ptr<ParameterCore> params_;

  // owns the ros_discovery_info publisher (defined in node.cpp to keep the
  // generated rmw_dds_common type out of the public header)
  class GraphAnnouncer;
  std::unique_ptr<GraphAnnouncer> discovery_;
};

}  // namespace rcl

#endif  // RCLLITE_NODE_HPP_
