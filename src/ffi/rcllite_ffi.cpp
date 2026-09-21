// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Python bindings for node initialization and execution, built on TVM FFI
// (the apache-tvm-ffi mechanism vila uses).  Compile as a shared library
// (linkshared = True) and load it from Python:
//
//   import tvm_ffi
//   mod = tvm_ffi.load_module("bazel-bin/src/ffi/rcllite_ffi.dll")
//   node = mod.node_create("talker", "/")
//   mod.spin_once(node, 100)
//
// Nodes are addressed by int64 handles into a process-wide registry; each
// node owns a dedicated Executor so Node.spin()/cancel() map 1:1 onto the
// rcl::Executor API.  The registry keeps the C++ objects alive while
// Python holds the handle; node_destroy() cancels a concurrent spin() before
// releasing the entry.
#include <tvm/ffi/tvm_ffi.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "rcllite/graph.hpp"
#include "rcllite/graph_executor.hpp"
#include "rcllite/rcllite.hpp"

namespace {

struct NodeEntry {
  std::shared_ptr<rcl::Node> node;
  std::unique_ptr<rcl::Executor> executor;
};

std::mutex g_registry_mtx;
std::unordered_map<int64_t, std::shared_ptr<NodeEntry>> g_registry;
int64_t g_next_handle = 1;  // never reused; 0 stays an invalid handle

std::shared_ptr<NodeEntry> lookup(int64_t handle) {
  std::lock_guard<std::mutex> lock(g_registry_mtx);
  const auto it = g_registry.find(handle);
  if (it == g_registry.end()) {
    TVM_FFI_THROW(ValueError) << "unknown node handle: " << handle;
  }
  return it->second;
}

int64_t NodeCreate_(const std::string& name, const std::string& namespace_) {
  auto entry = std::make_shared<NodeEntry>();
  entry->node = std::make_shared<rcl::Node>(name, namespace_);
  entry->executor = std::make_unique<rcl::Executor>();
  entry->executor->add_node(entry->node);

  std::lock_guard<std::mutex> lock(g_registry_mtx);
  const int64_t handle = g_next_handle++;
  g_registry.emplace(handle, std::move(entry));
  return handle;
}

void NodeDestroy_(int64_t handle) {
  std::shared_ptr<NodeEntry> entry;
  {
    std::lock_guard<std::mutex> lock(g_registry_mtx);
    const auto it = g_registry.find(handle);
    if (it == g_registry.end()) {
      return;  // idempotent destroy
    }
    entry = it->second;
    g_registry.erase(it);
  }
  // Unblocks a spin() running on another thread before the entry (and with
  // it the last reference outside that spin) goes away.
  entry->executor->cancel();
}

std::string NodeName_(int64_t handle) { return lookup(handle)->node->get_name(); }

std::string NodeNamespace_(int64_t handle) {
  return lookup(handle)->node->get_namespace();
}

std::string NodeFqName_(int64_t handle) {
  return lookup(handle)->node->get_fully_qualified_name();
}

void NodeSpin_(int64_t handle) { lookup(handle)->executor->spin(); }

bool NodeSpinOnce_(int64_t handle, int64_t timeout_ms) {
  return lookup(handle)->executor->spin_once(std::chrono::milliseconds(timeout_ms));
}

void NodeCancel_(int64_t handle) { lookup(handle)->executor->cancel(); }

std::mutex g_graph_mtx;
std::unique_ptr<rcl::GraphMonitor> g_graph;

void GraphStart_() {
  const std::lock_guard<std::mutex> lock(g_graph_mtx);
  if (!g_graph) {
    g_graph = std::make_unique<rcl::GraphMonitor>();
  }
}

void GraphStop_() {
  const std::lock_guard<std::mutex> lock(g_graph_mtx);
  g_graph.reset();
}

std::string GraphSnapshot_() {
  std::unique_lock<std::mutex> lock(g_graph_mtx);
  if (!g_graph) {
    TVM_FFI_THROW(ValueError) << "graph monitor not started";
  }
  // Release the registry lock while building the document: the monitor is
  // thread-safe internally and graph_start/stop stay responsive.
  rcl::GraphMonitor* monitor = g_graph.get();
  lock.unlock();
  return monitor->snapshot_json();
}

std::mutex g_graph_exec_mtx;
std::unordered_map<int64_t, std::unique_ptr<rcl::GraphExecutor>> g_graph_execs;
int64_t g_next_graph_exec = 1;

int64_t GraphExecCreate_(const std::string& json_topology) {
  // Build first so a config error throws before registering anything.
  auto executor = std::make_unique<rcl::GraphExecutor>(json_topology);
  const std::lock_guard<std::mutex> lock(g_graph_exec_mtx);
  const int64_t handle = g_next_graph_exec++;
  g_graph_execs.emplace(handle, std::move(executor));
  return handle;
}

rcl::GraphExecutor* graph_exec_lookup(int64_t handle) {
  const std::lock_guard<std::mutex> lock(g_graph_exec_mtx);
  const auto it = g_graph_execs.find(handle);
  if (it == g_graph_execs.end()) {
    TVM_FFI_THROW(ValueError) << "unknown graph executor handle: " << handle;
  }
  return it->second.get();
}

void GraphExecSpin_(int64_t handle) { graph_exec_lookup(handle)->spin(); }

bool GraphExecSpinOnce_(int64_t handle, int64_t timeout_ms) {
  return graph_exec_lookup(handle)->spin_once(std::chrono::milliseconds(timeout_ms));
}

void GraphExecCancel_(int64_t handle) { graph_exec_lookup(handle)->cancel(); }

std::string GraphExecStatus_(int64_t handle) {
  return graph_exec_lookup(handle)->status_json();
}

std::string GraphExecRegisteredNodes_() {
  return rcl::GraphExecutor::registered_nodes_json();
}

void GraphExecDestroy_(int64_t handle) {
  std::unique_ptr<rcl::GraphExecutor> executor;
  {
    const std::lock_guard<std::mutex> lock(g_graph_exec_mtx);
    const auto it = g_graph_execs.find(handle);
    if (it == g_graph_execs.end()) {
      return;  // idempotent destroy
    }
    executor = std::move(it->second);
    g_graph_execs.erase(it);
  }
  // Destroy outside the registry lock: teardown joins publisher threads.
  executor.reset();
}

}  // namespace

TVM_FFI_DLL_EXPORT_TYPED_FUNC(node_create, NodeCreate_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(node_destroy, NodeDestroy_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(node_name, NodeName_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(node_namespace, NodeNamespace_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(node_fully_qualified_name, NodeFqName_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(node_spin, NodeSpin_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(node_spin_once, NodeSpinOnce_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(node_cancel, NodeCancel_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(graph_start, GraphStart_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(graph_stop, GraphStop_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(graph_snapshot, GraphSnapshot_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(graph_exec_create, GraphExecCreate_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(graph_exec_spin, GraphExecSpin_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(graph_exec_spin_once, GraphExecSpinOnce_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(graph_exec_cancel, GraphExecCancel_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(graph_exec_status, GraphExecStatus_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(graph_exec_registered_nodes, GraphExecRegisteredNodes_)
TVM_FFI_DLL_EXPORT_TYPED_FUNC(graph_exec_destroy, GraphExecDestroy_)
