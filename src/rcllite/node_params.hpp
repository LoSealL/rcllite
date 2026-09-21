// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Template parameter conveniences on Node; included via rcllite.hpp.
#ifndef RCLLITE__NODE_PARAMS_HPP_
#define RCLLITE__NODE_PARAMS_HPP_

#include <string>
#include <utility>

#include "rcllite/exception.hpp"
#include "rcllite/node.hpp"
#include "rcllite/parameter.hpp"

namespace rcl {

template <typename T>
void Node::declare_parameter(const std::string& name, const T& default_value) {
  params().declare(name, param::to_value(default_value));
}

inline void Node::declare_parameter(const std::string& name) {
  param::ParameterValue not_set;
  params().declare(name, not_set);
}

inline param::ParameterValue Node::get_parameter(const std::string& name) const {
  return params().get(name);
}

template <typename T>
T Node::get_parameter_or(const std::string& name, const T& alternative) const {
  auto v = params().get_or(name);
  if (!v.has_value()) {
    return alternative;
  }
  return param::value_as<T>(*v);
}

template <typename T>
T Node::get_parameter(const std::string& name) const {
  auto v = params().get_or(name);
  if (!v.has_value()) {
    throw Error(vila::InvalidArguments("parameter not declared: {}", name));
  }
  return param::value_as<T>(*v);
}

inline bool Node::set_parameter(const std::string& name,
                                const param::ParameterValue& value) {
  return params().set(name, value);
}

}  // namespace rcl

#endif  // RCLLITE__NODE_PARAMS_HPP_
