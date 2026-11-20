// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#include "rcllite/node.hpp"

#include <algorithm>
#include <cstring>

#include "rcllite/exception.hpp"
#include "rcllite/logging.hpp"
#include "rcllite/names.hpp"
#include "rcllite/publisher.hpp"
#include "rcllite_types/rmw_dds_common/participant_entities_info.hpp"

namespace rcl {

Node::Node(const std::string& name) : Node(name, "/") {}

Node::Node(const std::string& name, const std::string& ns)
    : name_(name),
      ns_(ns.empty() ? "/" : ns),
      fq_name_(make_fq_node_name(ns_, name)),
      domain_id_(dds::Context::instance().domain_id()),
      change_guard_(std::make_unique<dds::GuardCondition>()) {
  if (name.empty() || name.find('/') != std::string::npos) {
    throw Error(vila::InvalidArguments("invalid node name: {}", name));
  }
  // The participant's USER_DATA advertises the node identity to the ROS 2
  // graph ("enclave=/ns/name;") so ros2 node list can discover it.
  ppant_ = std::make_unique<dds::Participant>(domain_id_, fq_name_);

  // Announce the node on "ros_discovery_info" following the
  // rmw_dds_common protocol every ROS 2 node uses; this is what makes the
  // node appear in `ros2 node list` and addressable by `ros2 param`.
  // rcllite reports no per-entity GIDs, so endpoint lists stay empty
  // (`ros2 node info` shows no publishers/subscriptions).
  announce_to_ros_graph();
}

class Node::GraphAnnouncer {
 public:
  explicit GraphAnnouncer(dds::Participant& ppant)
      : pub(ppant, "ros_discovery_info", QoS().set_transient_local().keep_last(1),
            /*raw_dds_topic=*/true) {}

  Publisher<rmw_dds_common::msg::ParticipantEntitiesInfo> pub;
};

void Node::announce_to_ros_graph() {
  using rmw_dds_common::msg::NodeEntitiesInfo;
  using rmw_dds_common::msg::ParticipantEntitiesInfo;

  dds_guid_t guid;
  if (dds_get_guid(ppant_->handle(), &guid) < 0) {
    throw Error(vila::InternalError("dds_get_guid failed for participant"));
  }

  auto announcer = std::make_unique<GraphAnnouncer>(*ppant_);
  discovery_ = std::move(announcer);

  ParticipantEntitiesInfo info;
  std::memcpy(info.gid.data.data(), guid.v, 16);
  NodeEntitiesInfo node_info;
  node_info.node_namespace = ns_;
  node_info.node_name = name_;
  info.node_entities_info_seq.push_back(std::move(node_info));
  discovery_->pub.publish(info);
}

Node::~Node() = default;

uint64_t Node::entity_epoch() const {
  std::lock_guard<std::mutex> lock(entities_mtx_);
  return entity_epoch_;
}

dds::GuardCondition& Node::change_guard() { return *change_guard_; }

std::vector<std::shared_ptr<EntityBase>> Node::entities() const {
  std::lock_guard<std::mutex> lock(entities_mtx_);
  return entities_;
}

void Node::add_entity(std::shared_ptr<EntityBase> entity) {
  {
    std::lock_guard<std::mutex> lock(entities_mtx_);
    entities_.push_back(std::move(entity));
    ++entity_epoch_;
  }
  change_guard_->trigger();
}

void Node::remove_entity(const std::shared_ptr<EntityBase>& entity) {
  {
    std::lock_guard<std::mutex> lock(entities_mtx_);
    entities_.erase(std::remove(entities_.begin(), entities_.end(), entity),
                    entities_.end());
    ++entity_epoch_;
  }
  change_guard_->trigger();
}

std::vector<std::string> Node::pub_topics() const {
  std::lock_guard<std::mutex> lock(entities_mtx_);
  return pub_topics_;
}

std::vector<std::string> Node::sub_topics() const {
  std::lock_guard<std::mutex> lock(entities_mtx_);
  return sub_topics_;
}

void Node::note_pub_topic(const std::string& fqn) {
  std::lock_guard<std::mutex> lock(entities_mtx_);
  pub_topics_.push_back(fqn);
}

void Node::note_sub_topic(const std::string& fqn) {
  std::lock_guard<std::mutex> lock(entities_mtx_);
  sub_topics_.push_back(fqn);
}

void Node::set_lifecycle_sink(LifecycleSink sink) {
  std::lock_guard<std::mutex> lock(lifecycle_mtx_);
  lifecycle_sink_ = std::move(sink);
}

void Node::mark_batch_finished() {
  std::function<void(Node&)> report;
  {
    std::lock_guard<std::mutex> lock(lifecycle_mtx_);
    report = lifecycle_sink_.batch_finished;
  }
  if (report) {
    report(*this);
  } else {
    RCLLITE_LOGW("{}: mark_batch_finished with no lifecycle sink installed",
                 fq_name_);
  }
}

void Node::request_graph_shutdown() {
  std::function<void()> request;
  {
    std::lock_guard<std::mutex> lock(lifecycle_mtx_);
    request = lifecycle_sink_.shutdown_requested;
  }
  if (request) {
    request();
  }
}

bool Node::wait_graph_active() {
  std::function<bool()> wait;
  {
    std::lock_guard<std::mutex> lock(lifecycle_mtx_);
    wait = lifecycle_sink_.wait_graph_active;
  }
  return !wait || wait();
}

}  // namespace rcl
