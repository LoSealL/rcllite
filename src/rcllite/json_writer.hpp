// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Tiny shared helpers for hand-building JSON documents (snapshot and status
// output).  Deliberately no JSON dependency on the write side.
#ifndef RCLLITE__JSON_WRITER_HPP_
#define RCLLITE__JSON_WRITER_HPP_

#include <string>

namespace rcl {

/// JSON string literal with quotes and escaping (control chars as \uXXXX).
inline std::string json_escape_string(const std::string& s) {
  static const char* hex = "0123456789abcdef";
  std::string out = "\"";
  for (const char c : s) {
    const unsigned char uc = static_cast<unsigned char>(c);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (uc < 0x20) {
          out += "\\u00";
          out += hex[uc >> 4];
          out += hex[uc & 0xf];
        } else {
          out += c;
        }
    }
  }
  out += '"';
  return out;
}

}  // namespace rcl

#endif  // RCLLITE__JSON_WRITER_HPP_
