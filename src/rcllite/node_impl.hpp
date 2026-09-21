// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Template factory method definitions for Node; included by rcllite.hpp
// after all entity headers (same pattern rclcpp uses).
#ifndef RCLLITE__NODE_IMPL_HPP_
#define RCLLITE__NODE_IMPL_HPP_

#include <memory>
#include <string>
#include <utility>

#include "rcllite/exception.hpp"
#include "rcllite/names.hpp"
#include "rcllite/node.hpp"
#include "rcllite/publisher.hpp"
#include "rcllite/service.hpp"
#include "rcllite/subscription.hpp"

namespace rcl {

template <typename MessageT>
std::shared_ptr<Publisher<MessageT>> Node::create_publisher(const std::string& topic,
                                                            const QoS& qos) {
  const std::string fqn = expand_topic_name(topic, ns_);
  return std::make_shared<Publisher<MessageT>>(*ppant_, fqn, qos);
}

template <typename MessageT, typename CallbackT>
std::shared_ptr<Subscription<MessageT>> Node::create_subscription(
    const std::string& topic, CallbackT&& callback, const QoS& qos) {
  const std::string fqn = expand_topic_name(topic, ns_);
  auto sub = std::make_shared<Subscription<MessageT>>(
      *ppant_, fqn,
      typename Subscription<MessageT>::Callback(std::forward<CallbackT>(callback)),
      qos);
  add_entity(sub);
  return sub;
}

template <typename ServiceT>
std::shared_ptr<Service<ServiceT>> Node::create_service(
    const std::string& service_name, typename Service<ServiceT>::Callback callback,
    const QoS& qos) {
  const std::string fqn = expand_topic_name(service_name, ns_);
  auto srv =
      std::make_shared<Service<ServiceT>>(*ppant_, fqn, std::move(callback), qos);
  add_entity(srv);
  return srv;
}

template <typename ServiceT>
std::shared_ptr<Client<ServiceT>> Node::create_client(const std::string& service_name,
                                                      const QoS& qos) {
  const std::string fqn = expand_topic_name(service_name, ns_);
  auto cli = std::make_shared<Client<ServiceT>>(*ppant_, fqn, qos);
  add_entity(cli);
  return cli;
}

}  // namespace rcl

#endif  // RCLLITE__NODE_IMPL_HPP_
