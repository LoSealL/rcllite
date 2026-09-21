// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#ifndef RCLLITE__NODE_HPP_
#define RCLLITE__NODE_HPP_

#include <cstdint>
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
  Node(const std::string& name, const std::string& namespace_);
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

  /// Access (lazily creating) the parameter support: the six parameter
  /// services plus the /parameter_events publisher.
  ParameterCore& params() const;

 private:
  friend class ParameterCore;

  std::string name_;
  std::string ns_;
  std::string fq_name_;
  uint32_t domain_id_;
  std::unique_ptr<dds::Participant> ppant_;

  mutable std::mutex entities_mtx_;
  std::vector<std::shared_ptr<EntityBase>> entities_;
  uint64_t entity_epoch_ = 0;
  std::unique_ptr<dds::GuardCondition> change_guard_;

  mutable std::mutex params_mtx_;
  mutable std::unique_ptr<ParameterCore> params_;

  // owns the ros_discovery_info publisher (defined in node.cpp to keep the
  // generated rmw_dds_common type out of the public header)
  class GraphAnnouncer;
  std::unique_ptr<GraphAnnouncer> discovery_;
};

}  // namespace rcl

#endif  // RCLLITE__NODE_HPP_
