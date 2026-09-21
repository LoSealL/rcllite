// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#include "rcllite/executor.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "rcllite/exception.hpp"
#include "rcllite/logging.hpp"

namespace rcl {

namespace {
constexpr dds_attach_t kCancelAttachment = 1;
constexpr dds_attach_t kWakeAttachment = 2;
}  // namespace

Executor::Executor() {
  waitset_ = dds_create_waitset(DDS_CYCLONEDDS_HANDLE);
  if (waitset_ < 0) {
    throw Error(vila::InternalError("dds_create_waitset failed"));
  }
  if (dds_waitset_attach(waitset_, cancel_guard_.handle(), kCancelAttachment) < 0 ||
      dds_waitset_attach(waitset_, wake_guard_.handle(), kWakeAttachment) < 0) {
    throw Error(vila::InternalError("failed to attach executor guards"));
  }
}

Executor::~Executor() {
  if (waitset_ >= 0) {
    dds_delete(waitset_);
  }
}

void Executor::add_node(std::shared_ptr<Node> node) {
  {
    std::lock_guard<std::mutex> lock(nodes_mtx_);
    if (std::find(nodes_.begin(), nodes_.end(), node) == nodes_.end()) {
      nodes_.push_back(node);
      node_epochs_.erase(node.get());
    }
    reconcile_locked();
  }
  // Interrupt a concurrently running wait so the new attachments take
  // effect immediately; the wake guard carries no cancel semantics.
  wake_guard_.trigger();
}

void Executor::remove_node(const std::shared_ptr<Node>& node) {
  std::lock_guard<std::mutex> lock(nodes_mtx_);
  nodes_.erase(std::remove(nodes_.begin(), nodes_.end(), node), nodes_.end());
  node_epochs_.erase(node.get());
  reconcile_locked();
  wake_guard_.trigger();
}

void Executor::reconcile_locked() {
  // Desired attachment set keyed by condition handle (unique per entity).
  std::map<dds_entity_t, Attached> desired;
  for (const auto& node : nodes_) {
    desired[node->change_guard().handle()] =
        Attached{node->change_guard().handle(), node, nullptr};
    for (const auto& entity : node->entities()) {
      desired[entity->condition()] = Attached{entity->condition(), node, entity};
    }
    node_epochs_[node.get()] = node->entity_epoch();
  }

  // Detach conditions that dropped out of the desired set.
  std::set<dds_entity_t> current;
  for (const auto& [id, att] : attached_) {
    current.insert(att.cond);
  }
  std::vector<dds_attach_t> stale;
  for (const auto& [id, att] : attached_) {
    if (desired.find(att.cond) == desired.end()) {
      dds_waitset_detach(waitset_, att.cond);
      stale.push_back(id);
    }
  }
  for (dds_attach_t id : stale) {
    attached_.erase(id);
  }

  // Attach conditions not yet attached; ids stay stable per attachment.
  for (const auto& [cond, att] : desired) {
    if (current.insert(cond).second) {
      ++next_attach_id_;
      if (dds_waitset_attach(waitset_, cond,
                             static_cast<dds_attach_t>(next_attach_id_)) < 0) {
        current.erase(cond);  // condition vanished concurrently; skip
        continue;
      }
      attached_[static_cast<dds_attach_t>(next_attach_id_)] = att;
    }
  }
}

bool Executor::wait_and_process(dds_duration_t timeout) {
  {
    std::lock_guard<std::mutex> lock(nodes_mtx_);
    bool changed = attached_.empty() && !nodes_.empty();
    for (const auto& node : nodes_) {
      auto it = node_epochs_.find(node.get());
      if (it == node_epochs_.end() || it->second != node->entity_epoch()) {
        changed = true;
        break;
      }
    }
    if (changed) {
      reconcile_locked();
    }
  }

  constexpr size_t MAX_TRIGGERS = 64;
  dds_attach_t triggered[MAX_TRIGGERS];
  const dds_return_t n = dds_waitset_wait(waitset_, triggered, MAX_TRIGGERS, timeout);
  if (n < 0) {
    throw Error(vila::InternalError("dds_waitset_wait failed: {}", n));
  }
  if (n == 0) {
    return false;
  }

  bool did_work = false;
  for (size_t i = 0; i < static_cast<size_t>(n); ++i) {
    const dds_attach_t id = triggered[i];
    if (id == kCancelAttachment) {
      cancel_guard_.take();
      cancel_requested_ = true;
    } else if (id == kWakeAttachment) {
      wake_guard_.take();
    } else {
      std::shared_ptr<EntityBase> entity;
      {
        std::lock_guard<std::mutex> lock(nodes_mtx_);
        const auto it = attached_.find(id);
        if (it != attached_.end()) {
          entity = it->second.entity;
        }
      }
      if (entity) {
        try {
          entity->dispatch();
        } catch (const std::exception& e) {
          RCLLITE_LOGE("dispatch error: {}", e.what());
        }
        did_work = true;
      }
    }
  }

  // Absorb node change guards; the next wait_and_process reconciles.
  {
    std::lock_guard<std::mutex> lock(nodes_mtx_);
    for (const auto& node : nodes_) {
      node->change_guard().take();
    }
  }
  return did_work;
}

void Executor::spin() {
  cancel_requested_ = false;
  while (!cancel_requested_.load()) {
    wait_and_process(DDS_INFINITY);
  }
}

bool Executor::spin_once(std::chrono::nanoseconds timeout) {
  const dds_duration_t dds_timeout =
      timeout.count() < 0 ? DDS_INFINITY : static_cast<dds_duration_t>(timeout.count());
  return wait_and_process(dds_timeout);
}

bool Executor::spin_some() {
  bool did_work = false;
  while (wait_and_process(0)) {
    did_work = true;
  }
  return did_work;
}

void Executor::cancel() {
  cancel_requested_ = true;
  cancel_guard_.trigger();
}

void spin(std::shared_ptr<Node> node) {
  Executor exec;
  exec.add_node(std::move(node));
  exec.spin();
}

}  // namespace rcl
