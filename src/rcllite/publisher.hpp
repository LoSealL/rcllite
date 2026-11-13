// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#ifndef RCLLITE_PUBLISHER_HPP_
#define RCLLITE_PUBLISHER_HPP_

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>

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
  /// With `QoS::wait_forever()`/`wait_for()` the call blocks while the
  /// in-flight window (keep-last depth) awaits reader acknowledgement —
  /// returns false if that wait timed out (nothing written).  Throws if the
  /// write itself fails.  Safe to call from any thread.
  bool publish(const MessageT& msg) {
    CdrWriter w;
    MessageT::serialize(msg, w);
    const dds::WriteStatus status =
        writer_->write(w.payload().data(), w.payload().size());
    if (status == dds::WriteStatus::Failed) {
      throw Error(vila::InternalError("publish failed on {}", ros_topic_));
    }
    return status == dds::WriteStatus::Ok;
  }

  /// Backpressure publish: wait up to `timeout` while no subscription is
  /// matched — including after the last one left — then write.  Samples
  /// written with no reader matched are invisible to later joiners (DDS
  /// volatile default); holding the message closes that gap, though not
  /// the inherent DDS race of a subscriber leaving again before the write.
  /// Returns false without writing if nobody matched within `timeout` —
  /// nothing is dropped and the caller can retry; throws like publish()
  /// if the write itself fails.  A publisher with a QoS wait policy may
  /// block further on acknowledgement after the subscriber appears.
  /// Transient-local publishers already deliver their history to late
  /// joiners and rarely need this.  Safe to call from any thread, but
  /// blocks the calling one: avoid it inside executor callbacks unless
  /// stalling the spin thread is intended.
  bool publish(const MessageT& msg, std::chrono::nanoseconds timeout) {
    if (!wait_for_subscribers(timeout)) {
      return false;
    }
    return publish(msg);
  }

  /// Number of currently matched subscriptions (discovered readers).
  size_t get_subscription_count() const { return writer_->reader_count(); }

  /// Block until at least one subscription is matched (same polling pattern
  /// as Client::wait_for_service).  Returns false if `timeout` expires first.
  bool wait_for_subscribers(
      std::chrono::nanoseconds timeout = std::chrono::seconds(10)) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (get_subscription_count() == 0) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
  }

  const std::string& get_topic_name() const { return ros_topic_; }

 private:
  std::string ros_topic_;
  std::unique_ptr<dds::Writer> writer_;
};

}  // namespace rcl

#endif  // RCLLITE_PUBLISHER_HPP_
