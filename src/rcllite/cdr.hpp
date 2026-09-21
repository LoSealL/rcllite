// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Minimal CDR (Common Data Representation) writer/reader that produces and
// consumes byte streams bit-identical to what ROS 2 nodes put on the wire:
//   * 4-byte encapsulation header, little-endian (0x00 0x01 0x00 0x00)
//   * alignment computed relative to the first byte after that header
//   * strings are uint32 length + bytes + NUL
//   * sequences are uint32 length + elements
//   * fixed-size arrays are stored without a length prefix
#ifndef RCLLITE__CDR_HPP_
#define RCLLITE__CDR_HPP_

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "rcllite/exception.hpp"

namespace rcl {

/// Serialized message buffer.  `payload()` includes the 4-byte CDR
/// encapsulation header, exactly like an rmw_serialized_message_t.
class CdrWriter {
 public:
  CdrWriter() {
    // CDR encapsulation header: CDR_LE, no options.
    buf_.push_back(0x00);
    buf_.push_back(0x01);
    buf_.push_back(0x00);
    buf_.push_back(0x00);
  }

  const std::vector<uint8_t>& payload() const { return buf_; }

  /// Current offset relative to the start of the payload (i.e. excluding the
  /// encapsulation header).  This is the value CDR alignment rules use.
  size_t payload_offset() const { return buf_.size() - 4; }

  void align_to(size_t n) {
    const size_t pad = (n - (payload_offset() % n)) % n;
    for (size_t i = 0; i < pad; ++i) {
      buf_.push_back(0);
    }
  }

  void write_bytes(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    buf_.insert(buf_.end(), p, p + size);
  }

  void write_aligned(const void* data, size_t size, size_t alignment) {
    align_to(alignment);
    write_bytes(data, size);
  }

  void write_bool(bool v) { write_aligned(&v, 1, 1); }
  void write_byte(uint8_t v) { write_aligned(&v, 1, 1); }
  void write_uint8(uint8_t v) { write_aligned(&v, 1, 1); }
  void write_int8(int8_t v) { write_aligned(&v, 1, 1); }
  void write_char(uint8_t v) { write_aligned(&v, 1, 1); }
  void write_uint16(uint16_t v) { write_aligned(&v, 2, 2); }
  void write_int16(int16_t v) { write_aligned(&v, 2, 2); }
  void write_uint32(uint32_t v) { write_aligned(&v, 4, 4); }
  void write_int32(int32_t v) { write_aligned(&v, 4, 4); }
  void write_float32(float v) { write_aligned(&v, 4, 4); }
  void write_uint64(uint64_t v) { write_aligned(&v, 8, 8); }
  void write_int64(int64_t v) { write_aligned(&v, 8, 8); }
  void write_float64(double v) { write_aligned(&v, 8, 8); }

  /// ROS 2 `string` / `string<=N`: uint32 length (including NUL) + data + NUL.
  void write_string(const std::string& s) {
    write_uint32(static_cast<uint32_t>(s.size() + 1));
    write_bytes(s.c_str(), s.size() + 1);
  }

 private:
  std::vector<uint8_t> buf_;
};

class CdrReader {
 public:
  explicit CdrReader(const void* data, size_t size)
      : buf_(static_cast<const uint8_t*>(data)), size_(size), pos_(0) {
    if (size < 4) {
      throw Error(
          vila::InvalidArguments("CDR buffer too small for encapsulation header"));
    }
    // The RTPS encapsulation identifier is always big endian on the wire:
    // 0x0001 = CDR_LE payload, 0x0000 = CDR_BE.  ROS 2 writes CDR_LE;
    // rcllite only parses little endian payloads.
    const uint16_t id = static_cast<uint16_t>((buf_[0] << 8) | buf_[1]);
    if (id != 0x0001) {
      throw Error(
          vila::InvalidArguments("unsupported CDR encapsulation (expected CDR_LE)"));
    }
    pos_ = 4;
  }

  explicit CdrReader(const std::vector<uint8_t>& payload)
      : CdrReader(payload.data(), payload.size()) {}

  size_t remaining() const { return size_ - pos_; }
  size_t payload_offset() const { return pos_ - 4; }

  void align_to(size_t n) {
    const size_t pad = (n - (payload_offset() % n)) % n;
    if (pos_ + pad > size_) {
      throw Error(vila::InvalidArguments("CDR alignment runs past end of buffer"));
    }
    pos_ += pad;
  }

  void read_bytes(void* out, size_t size) {
    if (pos_ + size > size_) {
      throw Error(vila::InvalidArguments("CDR read past end of buffer"));
    }
    std::memcpy(out, buf_ + pos_, size);
    pos_ += size;
  }

  template <typename T>
  T read_aligned() {
    align_to(sizeof(T));
    T v;
    read_bytes(&v, sizeof(T));
    return v;
  }

  bool read_bool() { return read_aligned<bool>(); }
  uint8_t read_byte() { return read_aligned<uint8_t>(); }
  uint8_t read_uint8() { return read_aligned<uint8_t>(); }
  int8_t read_int8() { return read_aligned<int8_t>(); }
  uint8_t read_char() { return read_aligned<uint8_t>(); }
  uint16_t read_uint16() { return read_aligned<uint16_t>(); }
  int16_t read_int16() { return read_aligned<int16_t>(); }
  uint32_t read_uint32() { return read_aligned<uint32_t>(); }
  int32_t read_int32() { return read_aligned<int32_t>(); }
  float read_float32() { return read_aligned<float>(); }
  uint64_t read_uint64() { return read_aligned<uint64_t>(); }
  int64_t read_int64() { return read_aligned<int64_t>(); }
  double read_float64() { return read_aligned<double>(); }

  std::string read_string() {
    const uint32_t len = read_uint32();
    if (len == 0 || pos_ + len > size_) {
      throw Error(vila::InvalidArguments("invalid string length in CDR stream"));
    }
    std::string s(reinterpret_cast<const char*>(buf_ + pos_), len - 1);
    pos_ += len;
    return s;
  }

 private:
  const uint8_t* buf_;
  size_t size_;
  size_t pos_;
};

}  // namespace rcl

#endif  // RCLLITE__CDR_HPP_
