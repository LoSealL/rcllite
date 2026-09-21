// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Logging facade over the vila logger: everything rcllite logs goes through
// the process-wide "rcllite" logger nested under the vila root logger, so
// applications control verbosity uniformly via vila::Logger::SetLoggerLevel.
#ifndef RCLLITE__LOGGING_HPP_
#define RCLLITE__LOGGING_HPP_

#include "vila/logging/logger.h"

namespace rcl {

/// The library-wide logger (created once, nested as "vila.rcllite").
inline vila::Logger* logger() {
  static vila::Logger* lg = ::vila::Logger::Get()->Nest("rcllite");
  return lg;
}

}  // namespace rcl

#define RCLLITE_LOGT(...) ::rcl::logger()->Trace(__VA_ARGS__)
#define RCLLITE_LOGD(...) ::rcl::logger()->Debug(__VA_ARGS__)
#define RCLLITE_LOGI(...) ::rcl::logger()->Info(__VA_ARGS__)
#define RCLLITE_LOGW(...) ::rcl::logger()->Warning(__VA_ARGS__)
#define RCLLITE_LOGE(...) ::rcl::logger()->Error(__VA_ARGS__)

#endif  // RCLLITE__LOGGING_HPP_
