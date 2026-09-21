// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// ROS 2 compatible node parameters:
//   * declare/get/set with rcl_interfaces type semantics
//   * the six parameter services every rcl node exposes
//     (describe_parameters, get_parameter_types, get_parameters,
//      list_parameters, set_parameters, set_parameters_atomically)
//   * /parameter_events (transient local) so ros2 param CLI sees changes
#ifndef RCLLITE__PARAMETER_HPP_
#define RCLLITE__PARAMETER_HPP_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rcl {

class Node;

namespace param {

/// Mirrors rcl_interfaces/msg/ParameterType.
enum class Type : uint8_t {
  NotSet = 0,
  Bool = 1,
  Integer = 2,
  Double = 3,
  String = 4,
  ByteArray = 5,
  BoolArray = 6,
  IntegerArray = 7,
  DoubleArray = 8,
  StringArray = 9,
};

/// Mirrors rcl_interfaces/msg/ParameterValue (variant storage).
struct ParameterValue {
  Type type = Type::NotSet;
  bool bool_value = false;
  int64_t integer_value = 0;
  double double_value = 0.0;
  std::string string_value;
  std::vector<uint8_t> byte_array_value;
  std::vector<bool> bool_array_value;
  std::vector<int64_t> integer_array_value;
  std::vector<double> double_array_value;
  std::vector<std::string> string_array_value;
};

inline ParameterValue to_value(bool v) {
  ParameterValue p;
  p.type = Type::Bool;
  p.bool_value = v;
  return p;
}
inline ParameterValue to_value(int64_t v) {
  ParameterValue p;
  p.type = Type::Integer;
  p.integer_value = v;
  return p;
}
// int literals need this exact overload: int->bool/int64_t/double are all
// "Conversion" rank, so without it to_value(42) would be ambiguous
inline ParameterValue to_value(int v) { return to_value(static_cast<int64_t>(v)); }
inline ParameterValue to_value(double v) {
  ParameterValue p;
  p.type = Type::Double;
  p.double_value = v;
  return p;
}
inline ParameterValue to_value(const char* v) {
  ParameterValue p;
  p.type = Type::String;
  p.string_value = v;
  return p;
}
inline ParameterValue to_value(const std::string& v) {
  ParameterValue p;
  p.type = Type::String;
  p.string_value = v;
  return p;
}
inline ParameterValue to_value(const std::vector<std::string>& v) {
  ParameterValue p;
  p.type = Type::StringArray;
  p.string_array_value = v;
  return p;
}

template <typename T>
T value_as(const ParameterValue& p) {
  static_assert(sizeof(T) == 0, "value_as<T>: unsupported type");
}

template <>
inline bool value_as<bool>(const ParameterValue& p) {
  return p.bool_value;
}
template <>
inline int64_t value_as<int64_t>(const ParameterValue& p) {
  return p.integer_value;
}
template <>
inline double value_as<double>(const ParameterValue& p) {
  return p.double_value;
}
template <>
inline std::string value_as<std::string>(const ParameterValue& p) {
  return p.string_value;
}

}  // namespace param

/// Non-template parameter core; instantiated lazily by Node.
class ParameterCore {
 public:
  explicit ParameterCore(Node& node);
  ~ParameterCore();

  void declare(const std::string& name, const param::ParameterValue& default_value);
  param::ParameterValue get(const std::string& name) const;
  /// Returns empty optional for undeclared parameters.
  std::optional<param::ParameterValue> get_or(const std::string& name) const;
  bool set(const std::string& name, const param::ParameterValue& value);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rcl

#endif  // RCLLITE__PARAMETER_HPP_
