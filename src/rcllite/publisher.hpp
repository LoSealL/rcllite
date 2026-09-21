// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#ifndef RCLLITE__PUBLISHER_HPP_
#define RCLLITE__PUBLISHER_HPP_

#include <memory>
#include <string>

#include "rcllite/cdr.hpp"
#include "rcllite/dds/entities.hpp"
#include "rcllite/exception.hpp"
#include "rcllite/msg_traits.hpp"
#include "rcllite/names.hpp"

namespace rcl {

template <typename MessageT>
class Publisher {
 public:
  /// `raw_dds_topic` bypasses the rt/ prefix mapping (used for the
  /// rmw_dds_common "ros_discovery_info" topic, mirroring rmw's
  /// avoid_ros_namespace_conventions).
  Publisher(dds::Participant& ppant, const std::string& ros_topic, const QoS& qos,
            bool raw_dds_topic = false)
      : ros_topic_(ros_topic) {
    writer_ = std::make_unique<dds::Writer>(
        ppant.handle(), ppant.publisher(),
        raw_dds_topic ? ros_topic : ros_to_dds_topic_name(ros_topic),
        MessageType<MessageT>::dds_name(), qos);
  }

  /// Serialize with the message's MessageType support and publish.
  /// Safe to call from any thread.
  void publish(const MessageT& msg) {
    CdrWriter w;
    MessageT::serialize(msg, w);
    if (!writer_->write(w.payload().data(), w.payload().size())) {
      throw Error(vila::InternalError("publish failed on {}", ros_topic_));
    }
  }

  const std::string& get_topic_name() const { return ros_topic_; }

 private:
  std::string ros_topic_;
  std::unique_ptr<dds::Writer> writer_;
};

}  // namespace rcl

#endif  // RCLLITE__PUBLISHER_HPP_
