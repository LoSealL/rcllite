// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Service servers and clients speaking the rmw_cyclonedds wire format:
//
//   request topic:  rq/<fqn>Request   type pkg::srv::dds_::Name_Request_
//   reply   topic:  rr/<fqn>Reply     type pkg::srv::dds_::Name_Reply_
//   payload:  [4B CDR encaps][8B client/writer id][8B sequence][message CDR]
//
// The 8-byte id is chosen by the client, echoed verbatim by the server, and
// used by the client to filter responses; the sequence number correlates a
// response with its request.  (The id-size difference is exactly why ROS 2
// services of different rmw implementations cannot talk to each other; by
// mirroring rmw_cyclonedds byte-for-byte we interoperate with every node
// running RMW_IMPLEMENTATION=rmw_cyclonedds_cpp.)
#ifndef RCLLITE__SERVICE_HPP_
#define RCLLITE__SERVICE_HPP_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "rcllite/cdr.hpp"
#include "rcllite/dds/entities.hpp"
#include "rcllite/exception.hpp"
#include "rcllite/logging.hpp"
#include "rcllite/msg_traits.hpp"
#include "rcllite/names.hpp"
#include "rcllite/node.hpp"

namespace rcl {

struct ServiceHeader {
  uint64_t client_id;
  int64_t sequence;
};

inline void write_service_header(CdrWriter& w, const ServiceHeader& h) {
  w.write_uint64(h.client_id);
  w.write_int64(h.sequence);
}

inline ServiceHeader read_service_header(CdrReader& r) {
  ServiceHeader h{};
  h.client_id = r.read_uint64();
  h.sequence = r.read_int64();
  return h;
}

template <typename ServiceT>
class Service : public EntityBase {
 public:
  using Request = typename ServiceT::Request;
  using Response = typename ServiceT::Response;
  using Callback = std::function<Response(const Request&)>;

  Service(dds::Participant& ppant, const std::string& ros_service, Callback callback,
          const QoS& qos)
      : ros_service_(ros_service), callback_(std::move(callback)) {
    request_reader_ = std::make_unique<dds::Reader>(
        ppant.handle(), ppant.subscriber(), ros_to_dds_request_topic_name(ros_service),
        MessageType<Request>::dds_name(), qos);
    reply_writer_ = std::make_unique<dds::Writer>(
        ppant.handle(), ppant.publisher(), ros_to_dds_reply_topic_name(ros_service),
        MessageType<Response>::dds_name(), qos);
  }

  const std::string& get_service_name() const { return ros_service_; }

  dds_entity_t condition() const override { return request_reader_->read_condition(); }

  /// Drain pending requests: deserialize, run the callback, send the reply
  /// with the request's header echoed back.
  void dispatch() override {
    request_reader_->take([this](const uint8_t* payload, size_t size,
                                 const dds_sample_info_t&) {
      try {
        CdrReader r(payload, size);
        const ServiceHeader h = read_service_header(r);
        Request req{};
        if (!Request::deserialize(req, r)) {
          return;
        }
        Response resp = callback_(req);
        CdrWriter w;
        write_service_header(w, h);
        Response::serialize(resp, w);
        reply_writer_->write(w.payload().data(), w.payload().size());
      } catch (const std::exception& e) {
        RCLLITE_LOGE("failed to handle request: {} (payload {} bytes)", e.what(), size);
      }
    });
  }

 private:
  std::string ros_service_;
  Callback callback_;
  std::unique_ptr<dds::Reader> request_reader_;
  std::unique_ptr<dds::Writer> reply_writer_;
};

template <typename ServiceT>
class Client : public EntityBase {
 public:
  using Request = typename ServiceT::Request;
  using Response = typename ServiceT::Response;

  explicit Client(dds::Participant& ppant, const std::string& ros_service,
                  const QoS& qos)
      : ros_service_(ros_service),
        client_id_((static_cast<uint64_t>(std::random_device{}()) << 32) |
                   static_cast<uint64_t>(std::random_device{}())) {
    request_writer_ = std::make_unique<dds::Writer>(
        ppant.handle(), ppant.publisher(), ros_to_dds_request_topic_name(ros_service),
        MessageType<Request>::dds_name(), qos);
    response_reader_ = std::make_unique<dds::Reader>(
        ppant.handle(), ppant.subscriber(), ros_to_dds_reply_topic_name(ros_service),
        MessageType<Response>::dds_name(), qos);
  }

  const std::string& get_service_name() const { return ros_service_; }

  bool service_is_ready() const { return response_reader_->writer_count() > 0; }

  /// Block until a matching service server is discovered.
  bool wait_for_service(
      std::chrono::nanoseconds timeout = std::chrono::seconds(10)) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!service_is_ready()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
  }

  /// Asynchronously send a request; `on_response` runs on the spinning
  /// executor thread once the response arrives.
  /// Returns the sequence number that will correlate the response.
  int64_t async_send_request(
      const Request& request,
      std::function<void(const Response&)> on_response = nullptr) {
    return send_request(request, std::move(on_response), nullptr);
  }

  /// Synchronous call.  Requires an Executor spinning this client's node on
  /// another thread (otherwise no response can ever be received).
  Response call(const Request& request,
                std::chrono::nanoseconds timeout = std::chrono::seconds(30)) {
    std::promise<Response> promise;
    std::future<Response> future = promise.get_future();
    const int64_t seq =
        send_request(request, std::function<void(const Response&)>(), &promise);
    if (future.wait_for(timeout) != std::future_status::ready) {
      std::lock_guard<std::mutex> lock(pending_mtx_);
      pending_.erase(seq);
      throw Error(
          vila::WaitTimeout("timeout waiting for response from {}", ros_service_));
    }
    return future.get();
  }

  dds_entity_t condition() const override { return response_reader_->read_condition(); }

  /// Drain pending responses and complete the matching futures/callbacks.
  void dispatch() override {
    response_reader_->take(
        [this](const uint8_t* payload, size_t size, const dds_sample_info_t&) {
          try {
            CdrReader r(payload, size);
            const ServiceHeader h = read_service_header(r);
            if (h.client_id != client_id_) {
              return;  // response to some other client
            }
            Response resp{};
            if (!Response::deserialize(resp, r)) {
              return;
            }
            Pending p;
            bool have_pending = false;
            {
              std::lock_guard<std::mutex> lock(pending_mtx_);
              auto it = pending_.find(h.sequence);
              if (it != pending_.end()) {
                p = std::move(it->second);
                pending_.erase(it);
                have_pending = true;
              }
            }
            if (have_pending) {
              if (p.sync_promise != nullptr) {
                p.sync_promise->set_value(resp);
              }
              if (p.on_response) {
                p.on_response(resp);
              }
            }
          } catch (const std::exception& e) {
            RCLLITE_LOGE("failed to handle response: {}", e.what());
          }
        });
  }

 private:
  struct Pending {
    std::function<void(const Response&)> on_response;
    std::promise<Response>* sync_promise = nullptr;  // set by call()
  };

  /// Atomically allocate the sequence number, register the completion and
  /// write the request, so responses can never race the registration.
  int64_t send_request(const Request& request,
                       std::function<void(const Response&)> on_response,
                       std::promise<Response>* sync_promise) {
    const int64_t seq = ++next_sequence_;
    {
      std::lock_guard<std::mutex> lock(pending_mtx_);
      pending_[seq] = Pending{std::move(on_response), sync_promise};
    }

    CdrWriter w;
    write_service_header(w, ServiceHeader{client_id_, seq});
    Request::serialize(request, w);
    if (!request_writer_->write(w.payload().data(), w.payload().size())) {
      std::lock_guard<std::mutex> lock(pending_mtx_);
      pending_.erase(seq);
      throw Error(vila::InternalError("failed to send request on {}", ros_service_));
    }
    return seq;
  }

  std::string ros_service_;
  uint64_t client_id_;
  std::atomic<int64_t> next_sequence_{0};
  std::unique_ptr<dds::Writer> request_writer_;
  std::unique_ptr<dds::Reader> response_reader_;

  std::mutex pending_mtx_;
  std::map<int64_t, Pending> pending_;
};

}  // namespace rcl

#endif  // RCLLITE__SERVICE_HPP_
