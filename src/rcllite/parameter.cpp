// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#include "rcllite/parameter.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <utility>

#include "rcllite/exception.hpp"
#include "rcllite/node.hpp"
#include "rcllite/publisher.hpp"
#include "rcllite/service.hpp"
#include "rcllite_types/builtin_interfaces/time.hpp"
#include "rcllite_types/rcl_interfaces/describe_parameters.hpp"
#include "rcllite_types/rcl_interfaces/get_parameter_types.hpp"
#include "rcllite_types/rcl_interfaces/get_parameters.hpp"
#include "rcllite_types/rcl_interfaces/list_parameters.hpp"
#include "rcllite_types/rcl_interfaces/parameter_event.hpp"
#include "rcllite_types/rcl_interfaces/set_parameters.hpp"
#include "rcllite_types/rcl_interfaces/set_parameters_atomically.hpp"

namespace rcl {

using rcl_interfaces::msg::ParameterEvent;
using RclParameter = rcl_interfaces::msg::Parameter;
using RclParameterValue = rcl_interfaces::msg::ParameterValue;
using RclSetParametersResult = rcl_interfaces::msg::SetParametersResult;

namespace {

RclParameterValue to_rcl_value(const param::ParameterValue& v) {
  RclParameterValue r;
  r.type = static_cast<uint8_t>(v.type);
  r.bool_value = v.bool_value;
  r.integer_value = v.integer_value;
  r.double_value = v.double_value;
  r.string_value = v.string_value;
  r.byte_array_value = v.byte_array_value;
  r.bool_array_value = v.bool_array_value;
  r.integer_array_value = v.integer_array_value;
  r.double_array_value = v.double_array_value;
  r.string_array_value = v.string_array_value;
  return r;
}

param::ParameterValue from_rcl_value(const RclParameterValue& v) {
  param::ParameterValue p;
  p.type = static_cast<param::Type>(v.type);
  p.bool_value = v.bool_value;
  p.integer_value = v.integer_value;
  p.double_value = v.double_value;
  p.string_value = v.string_value;
  p.byte_array_value = v.byte_array_value;
  p.bool_array_value = v.bool_array_value;
  p.integer_array_value = v.integer_array_value;
  p.double_array_value = v.double_array_value;
  p.string_array_value = v.string_array_value;
  return p;
}

builtin_interfaces::msg::Time now_stamp() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  builtin_interfaces::msg::Time t;
  t.sec = static_cast<int32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(now).count());
  t.nanosec = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() % 1000000000LL);
  return t;
}

/// Dot-hierarchy helpers for ListParameters ("a.b.c" naming like rcl).
std::vector<std::string> tokenize(const std::string& name) {
  std::vector<std::string> parts;
  std::string cur;
  std::istringstream is(name);
  while (std::getline(is, cur, '.')) {
    parts.push_back(cur);
  }
  return parts;
}

}  // namespace

struct ParameterCore::Impl {
 public:
  explicit Impl(Node& node) : node_(node) {
    events_pub_ = std::make_shared<Publisher<ParameterEvent>>(
        node.participant(), "/parameter_events", QoS::ParameterEvents());

    const std::string prefix = node.get_fully_qualified_name() + "/";
    using rcl_interfaces::srv::DescribeParameters;
    using rcl_interfaces::srv::GetParameters;
    using rcl_interfaces::srv::GetParameterTypes;
    using rcl_interfaces::srv::ListParameters;
    using rcl_interfaces::srv::SetParameters;
    using rcl_interfaces::srv::SetParametersAtomically;

    node.add_entity(std::make_shared<Service<DescribeParameters>>(
        node.participant(), prefix + "describe_parameters",
        [this](const DescribeParameters::Request& req) { return handle_describe(req); },
        QoS::Parameters()));

    node.add_entity(std::make_shared<Service<GetParameterTypes>>(
        node.participant(), prefix + "get_parameter_types",
        [this](const GetParameterTypes::Request& req) { return handle_get_types(req); },
        QoS::Parameters()));

    node.add_entity(std::make_shared<Service<GetParameters>>(
        node.participant(), prefix + "get_parameters",
        [this](const GetParameters::Request& req) { return handle_get(req); },
        QoS::Parameters()));

    node.add_entity(std::make_shared<Service<ListParameters>>(
        node.participant(), prefix + "list_parameters",
        [this](const ListParameters::Request& req) { return handle_list(req); },
        QoS::Parameters()));

    node.add_entity(std::make_shared<Service<SetParameters>>(
        node.participant(), prefix + "set_parameters",
        [this](const SetParameters::Request& req) { return handle_set(req); },
        QoS::Parameters()));

    node.add_entity(std::make_shared<Service<SetParametersAtomically>>(
        node.participant(), prefix + "set_parameters_atomically",
        [this](const SetParametersAtomically::Request& req) {
          return handle_set_atomically(req);
        },
        QoS::Parameters()));
  }

  void declare(const std::string& name, const param::ParameterValue& default_value) {
    if (name.empty()) {
      throw Error(vila::InvalidArguments("parameter name must not be empty"));
    }
    std::lock_guard<std::mutex> lock(mtx_);
    if (values_.find(name) != values_.end()) {
      throw Error(vila::InvalidArguments("parameter already declared: {}", name));
    }
    values_[name] = default_value;
    if (default_value.type != param::Type::NotSet) {
      publish_event_locked(name, default_value, true);
    }
  }

  param::ParameterValue get(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = values_.find(name);
    if (it == values_.end()) {
      throw Error(vila::InvalidArguments("parameter not declared: {}", name));
    }
    return it->second;
  }

  std::optional<param::ParameterValue> get_or(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = values_.find(name);
    if (it == values_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  bool set(const std::string& name, const param::ParameterValue& value) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = values_.find(name);
    if (it == values_.end()) {
      return false;  // undeclared parameters are rejected (rcl default)
    }
    const bool was_not_set = it->second.type == param::Type::NotSet;
    it->second = value;
    if (value.type != param::Type::NotSet) {
      publish_event_locked(name, value, was_not_set);
    }
    return true;
  }

 private:
  void publish_event_locked(const std::string& name, const param::ParameterValue& value,
                            bool is_new) {
    ParameterEvent ev;
    ev.stamp = now_stamp();
    ev.node = node_.get_fully_qualified_name();
    RclParameter p;
    p.name = name;
    p.value = to_rcl_value(value);
    if (is_new) {
      ev.new_parameters.push_back(std::move(p));
    } else {
      ev.changed_parameters.push_back(std::move(p));
    }
    events_pub_->publish(ev);
  }

  rcl_interfaces::srv::DescribeParameters::Response handle_describe(
      const rcl_interfaces::srv::DescribeParameters::Request& req) {
    using rcl_interfaces::msg::ParameterDescriptor;
    rcl_interfaces::srv::DescribeParameters::Response resp;
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& name : req.names) {
      ParameterDescriptor d;
      d.name = name;
      auto it = values_.find(name);
      d.type = it != values_.end() ? static_cast<uint8_t>(it->second.type)
                                   : static_cast<uint8_t>(param::Type::NotSet);
      d.dynamic_typing = false;
      resp.descriptors.push_back(std::move(d));
    }
    return resp;
  }

  rcl_interfaces::srv::GetParameterTypes::Response handle_get_types(
      const rcl_interfaces::srv::GetParameterTypes::Request& req) {
    rcl_interfaces::srv::GetParameterTypes::Response resp;
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& name : req.names) {
      auto it = values_.find(name);
      resp.types.push_back(it != values_.end()
                               ? static_cast<uint8_t>(it->second.type)
                               : static_cast<uint8_t>(param::Type::NotSet));
    }
    return resp;
  }

  rcl_interfaces::srv::GetParameters::Response handle_get(
      const rcl_interfaces::srv::GetParameters::Request& req) {
    rcl_interfaces::srv::GetParameters::Response resp;
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& name : req.names) {
      auto it = values_.find(name);
      resp.values.push_back(it != values_.end() ? to_rcl_value(it->second)
                                                : RclParameterValue{});
    }
    return resp;
  }

  rcl_interfaces::srv::ListParameters::Response handle_list(
      const rcl_interfaces::srv::ListParameters::Request& req) {
    rcl_interfaces::srv::ListParameters::Response resp;
    const uint64_t depth = req.depth;  // 0 == DEPTH_RECURSIVE
    std::vector<std::string> matched;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      for (const auto& [name, value] : values_) {
        static_cast<void>(value);
        const auto parts = tokenize(name);
        bool ok = true;
        if (!req.prefixes.empty()) {
          ok = false;
          for (const auto& prefix : req.prefixes) {
            const auto pp = tokenize(prefix);
            if (parts.size() < pp.size()) {
              continue;
            }
            bool prefix_match = std::equal(pp.begin(), pp.end(), parts.begin());
            if (prefix_match) {
              const size_t below = parts.size() - pp.size();
              ok = depth == 0 || below <= depth;
              break;
            }
          }
        } else {
          ok = depth == 0 || parts.size() <= depth;
        }
        if (ok) {
          matched.push_back(name);
        }
      }
    }
    std::sort(matched.begin(), matched.end());
    resp.result.names = matched;

    // distinct intermediate prefixes among the matched names
    std::set<std::string> prefixes;
    for (const auto& name : matched) {
      for (size_t dot = name.find('.'); dot != std::string::npos;
           dot = name.find('.', dot + 1)) {
        prefixes.insert(name.substr(0, dot));
      }
    }
    resp.result.prefixes.assign(prefixes.begin(), prefixes.end());
    return resp;
  }

  rcl_interfaces::srv::SetParameters::Response handle_set(
      const rcl_interfaces::srv::SetParameters::Request& req) {
    rcl_interfaces::srv::SetParameters::Response resp;
    for (const auto& p : req.parameters) {
      const bool ok = set(p.name, from_rcl_value(p.value));
      RclSetParametersResult r;
      r.successful = ok;
      if (!ok) {
        r.reason = "parameter not declared";
      }
      resp.results.push_back(std::move(r));
    }
    return resp;
  }

  rcl_interfaces::srv::SetParametersAtomically::Response handle_set_atomically(
      const rcl_interfaces::srv::SetParametersAtomically::Request& req) {
    rcl_interfaces::srv::SetParametersAtomically::Response resp;
    RclSetParametersResult r;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      bool all_declared = true;
      for (const auto& p : req.parameters) {
        if (values_.find(p.name) == values_.end()) {
          all_declared = false;
          r.reason += (r.reason.empty() ? "" : ", ") + p.name;
        }
      }
      if (!all_declared) {
        r.reason = "parameters not declared: " + r.reason;
        resp.result = r;
        return resp;
      }
    }
    for (const auto& p : req.parameters) {
      set(p.name, from_rcl_value(p.value));
    }
    r.successful = true;
    resp.result = r;
    return resp;
  }

  Node& node_;
  std::shared_ptr<Publisher<ParameterEvent>> events_pub_;
  mutable std::mutex mtx_;
  std::map<std::string, param::ParameterValue> values_;
};

ParameterCore::ParameterCore(Node& node) : impl_(std::make_unique<Impl>(node)) {}

ParameterCore::~ParameterCore() = default;

void ParameterCore::declare(const std::string& name,
                            const param::ParameterValue& default_value) {
  impl_->declare(name, default_value);
}

param::ParameterValue ParameterCore::get(const std::string& name) const {
  return impl_->get(name);
}

std::optional<param::ParameterValue> ParameterCore::get_or(
    const std::string& name) const {
  return impl_->get_or(name);
}

bool ParameterCore::set(const std::string& name, const param::ParameterValue& value) {
  return impl_->set(name, value);
}

ParameterCore& Node::params() const {
  std::lock_guard<std::mutex> lock(params_mtx_);
  if (params_ == nullptr) {
    // ParameterCore registers services on this node, which requires
    // non-const access; nodes are handed around as shared_ptr<Node>.
    params_ =
        std::unique_ptr<ParameterCore>(new ParameterCore(const_cast<Node&>(*this)));
  }
  return *params_;
}

}  // namespace rcl
