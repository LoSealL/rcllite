// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#ifndef RCLLITE__EXCEPTION_HPP_
#define RCLLITE__EXCEPTION_HPP_

#include <stdexcept>
#include <utility>

#include "vila/status/status.h"

namespace rcl {

/// Runtime error carrying a vila::Status: the error code comes from the
/// vila::ErrorCode space (vila::InvalidArguments, vila::InternalError, ...)
/// and the message is fmt-formatted by the vila::Status constructors.
class Error : public std::runtime_error {
 public:
  explicit Error(vila::Status status)
      : std::runtime_error(status.ToString()), status_(std::move(status)) {}

  const vila::Status& status() const { return status_; }

 private:
  vila::Status status_;
};

}  // namespace rcl

#endif  // RCLLITE__EXCEPTION_HPP_
