// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// A -> B -> C pipeline driven by a GraphExecutor topology:
//
//   NodeA: no input, publishes 1, 2, 3, ... on /counter
//   NodeB: subscribes /counter, publishes the running sum on /sum
//   NodeC: subscribes /sum, prints each value
//
// All three are behavior-carrying Node subclasses registered with
// RCLLITE_REGISTER_NODE; the topology below instantiates them by "class"
// name, exactly like a user's plugin nodes.  Run:
//
//   bazelisk run //examples/graph:abc_graph [-- seconds]
//
// The main thread spins the executor for a few seconds, then verifies the
// pipeline: B's inputs must be consecutive numbers from A, and C's received
// sums must be a suffix of B's published sums (reliable QoS loses nothing
// after endpoints match; early samples may be lost to discovery races).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rcllite/graph_executor.hpp"
#include "rcllite/node_registry.hpp"
#include "rcllite/rcllite.hpp"
#include "rcllite_types/rcl_interfaces/parameter.hpp"

namespace {

using namespace std::chrono_literals;
using rcl_interfaces::msg::Parameter;

Parameter int_message(const std::string& name, int64_t value) {
  Parameter m;
  m.name = name;
  m.value.type = static_cast<uint8_t>(rcl::param::Type::Integer);
  m.value.integer_value = value;
  return m;
}

/// Publishes 1, 2, 3, ... every 300 ms (after a short discovery warmup).
class NodeA : public rcl::Node {
 public:
  NodeA(const std::string& name, const std::string& ns) : rcl::Node(name, ns) {
    pub_ = create_publisher<Parameter>("counter");
    thread_ = std::thread([this] { run(); });
  }

  ~NodeA() {
    stop_ = true;
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  static int64_t published() { return published_.load(); }

 private:
  void run() {
    std::this_thread::sleep_for(500ms);  // let A<->B endpoints match first
    int64_t n = 0;
    while (!stop_.load()) {
      pub_->publish(int_message("counter", ++n));
      published_.store(n);
      std::this_thread::sleep_for(300ms);
    }
  }

  std::shared_ptr<rcl::Publisher<Parameter>> pub_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  static std::atomic<int64_t> published_;
};

std::atomic<int64_t> NodeA::published_{0};

/// Accumulates every received number; publishes the running sum.
class NodeB : public rcl::Node {
 public:
  NodeB(const std::string& name, const std::string& ns) : rcl::Node(name, ns) {
    sum_pub_ = create_publisher<Parameter>("sum");
    create_subscription<Parameter>("counter", [this](const Parameter& m) {
      const int64_t x = m.value.integer_value;
      const int64_t sum = sum_.fetch_add(x) + x;
      if (first_input_ == 0) {
        first_input_.store(x);
      }
      last_input_.store(x);
      ++inputs_;
      published_sums_.push_back(sum);
      sum_pub_->publish(int_message("sum", sum));
    });
  }

  static int64_t inputs() { return inputs_.load(); }
  static int64_t first_input() { return first_input_.load(); }
  static int64_t last_input() { return last_input_.load(); }
  static std::vector<int64_t> published_sums() {
    const std::lock_guard<std::mutex> lock(sums_mtx_);
    return published_sums_;
  }

 private:
  std::shared_ptr<rcl::Publisher<Parameter>> sum_pub_;
  static std::atomic<int64_t> sum_;
  static std::atomic<int64_t> inputs_;
  static std::atomic<int64_t> first_input_;
  static std::atomic<int64_t> last_input_;
  static std::mutex sums_mtx_;
  static std::vector<int64_t> published_sums_;
};

std::atomic<int64_t> NodeB::sum_{0};
std::atomic<int64_t> NodeB::inputs_{0};
std::atomic<int64_t> NodeB::first_input_{0};
std::atomic<int64_t> NodeB::last_input_{0};
std::mutex NodeB::sums_mtx_;
std::vector<int64_t> NodeB::published_sums_;

/// Prints every received sum.
class NodeC : public rcl::Node {
 public:
  NodeC(const std::string& name, const std::string& ns) : rcl::Node(name, ns) {
    create_subscription<Parameter>("sum", [](const Parameter& m) {
      const int64_t sum = m.value.integer_value;
      std::printf("C: sum = %ld\n", static_cast<long>(sum));
      std::fflush(stdout);
      const std::lock_guard<std::mutex> lock(received_mtx_);
      received_.push_back(sum);
    });
  }

  static std::vector<int64_t> received() {
    const std::lock_guard<std::mutex> lock(received_mtx_);
    return received_;
  }

 private:
  static std::mutex received_mtx_;
  static std::vector<int64_t> received_;
};

std::mutex NodeC::received_mtx_;
std::vector<int64_t> NodeC::received_;

// The whole pipeline as a topology: structure here, behavior in the classes.
constexpr const char* kTopology = R"({
  "nodes": [
    {"name": "node_a", "class": "NodeA"},
    {"name": "node_b", "class": "NodeB"},
    {"name": "node_c", "class": "NodeC"}
  ]
})";

/// C's list must be a suffix of B's list (reliable after matching; early
/// samples can be lost while endpoints are still discovering each other).
bool received_is_suffix(const std::vector<int64_t>& published,
                        const std::vector<int64_t>& received) {
  if (received.size() > published.size() || received.empty()) {
    return false;
  }
  const size_t offset = published.size() - received.size();
  for (size_t i = 0; i < received.size(); ++i) {
    if (published[offset + i] != received[i]) {
      return false;
    }
  }
  return true;
}

}  // namespace

RCLLITE_REGISTER_NODE(NodeA);
RCLLITE_REGISTER_NODE(NodeB);
RCLLITE_REGISTER_NODE(NodeC);

int main(int argc, char** argv) {
  const int seconds = argc > 1 ? std::atoi(argv[1]) : 5;
  std::printf("registered classes: %s\n",
              rcl::GraphExecutor::registered_nodes_json().c_str());
  std::fflush(stdout);

  rcl::GraphExecutor executor(kTopology);
  std::thread spinner([&executor] { executor.spin(); });
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  executor.cancel();
  spinner.join();

  const int64_t a_published = NodeA::published();
  const int64_t b_inputs = NodeB::inputs();
  const auto b_sums = NodeB::published_sums();
  const auto c_received = NodeC::received();

  std::printf(
      "\nA published 1..%ld | B received %ld inputs (%ld..%ld) | "
      "B published %zu sums | C received %zu\n",
      static_cast<long>(a_published), static_cast<long>(b_inputs),
      static_cast<long>(NodeB::first_input()), static_cast<long>(NodeB::last_input()),
      b_sums.size(), c_received.size());

  bool ok = true;
  // B saw consecutive numbers, a contiguous window of A's stream.
  ok &= b_inputs > 0;
  ok &= NodeB::last_input() - NodeB::first_input() + 1 == b_inputs;
  ok &= NodeB::first_input() >= 1 && NodeB::last_input() <= a_published;
  // B's own sums are the running sum of its inputs.
  int64_t expect = 0;
  for (int64_t x = NodeB::first_input(); x <= NodeB::last_input(); ++x) {
    expect += x;
    const size_t idx = static_cast<size_t>(x - NodeB::first_input());
    ok &= idx < b_sums.size() && b_sums[idx] == expect;
  }
  // C saw exactly what B published (up to discovery-loss prefix).
  ok &= received_is_suffix(b_sums, c_received);

  std::printf("%s\n", ok ? "PIPELINE OK" : "PIPELINE FAILED");
  return ok ? 0 : 1;
}
