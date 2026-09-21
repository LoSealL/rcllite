// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#ifndef RCLLITE__SUBSCRIPTION_HPP_
#define RCLLITE__SUBSCRIPTION_HPP_

#include <functional>
#include <memory>
#include <string>

#include "rcllite/cdr.hpp"
#include "rcllite/dds/entities.hpp"
#include "rcllite/exception.hpp"
#include "rcllite/logging.hpp"
#include "rcllite/msg_traits.hpp"
#include "rcllite/node.hpp"

namespace rcl {

template <typename MessageT>
class Subscription : public EntityBase {
 public:
  using Callback = std::function<void(const MessageT&)>;

  Subscription(dds::Participant& ppant, const std::string& ros_topic, Callback callback,
               const QoS& qos)
      : ros_topic_(ros_topic), callback_(std::move(callback)) {
    reader_ = std::make_unique<dds::Reader>(ppant.handle(), ppant.subscriber(),
                                            ros_to_dds_topic_name(ros_topic),
                                            MessageType<MessageT>::dds_name(), qos);
  }

  const std::string& get_topic_name() const { return ros_topic_; }

  dds_entity_t condition() const override { return reader_->read_condition(); }

  /// Drain all received samples, deserializing and invoking the callback.
  void dispatch() override {
    reader_->take(
        [this](const uint8_t* payload, size_t size, const dds_sample_info_t&) {
          try {
            CdrReader r(payload, size);
            MessageT msg{};
            if (MessageT::deserialize(msg, r)) {
              callback_(msg);
            }
          } catch (const std::exception& e) {
            RCLLITE_LOGW("dropping malformed sample: {}", e.what());
          }
        });
  }

 private:
  std::string ros_topic_;
  Callback callback_;
  std::unique_ptr<dds::Reader> reader_;
};

}  // namespace rcl

#endif  // RCLLITE__SUBSCRIPTION_HPP_
