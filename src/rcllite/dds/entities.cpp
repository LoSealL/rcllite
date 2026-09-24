// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#include "rcllite/dds/entities.hpp"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <thread>

#include "rcllite/dds/raw_serdata.hpp"
#include "rcllite/exception.hpp"

namespace rcl {
namespace dds {

namespace {

std::once_flag g_ctx_once;
Context* g_ctx = nullptr;

uint32_t env_domain_id() {
  const char* env = std::getenv("ROS_DOMAIN_ID");
  if (env == nullptr || *env == '\0') {
    return 0;
  }
  const long v = std::strtol(env, nullptr, 10);
  if (v < 0 || v > 232) {
    return 0;
  }
  return static_cast<uint32_t>(v);
}

bool env_localhost_only() {
  const char* env = std::getenv("ROS_LOCALHOST_ONLY");
  return env != nullptr && (std::string(env) == "1" || std::string(env) == "true");
}

dds_qos_t* base_dds_qos() {
  dds_qos_t* q = dds_create_qos();
  if (q == nullptr) {
    throw Error(vila::InternalError("dds_create_qos failed"));
  }
  return q;
}

}  // namespace

Context& Context::instance() {
  std::call_once(g_ctx_once, [] { g_ctx = new Context(); });
  return *g_ctx;
}

void Context::shutdown() {
  // The CycloneDDS domain tears down automatically when the process exits;
  // rcllite keeps no extra state here.
}

Context::Context()
    : domain_id_(env_domain_id()), localhost_only_(env_localhost_only()) {}

std::string default_config_xml(bool localhost_only) {
  // When the user provided CYCLONEDDS_URI we leave everything to it;
  // otherwise constrain discovery to localhost when ROS_LOCALHOST_ONLY=1,
  // using the same recipe rmw_cyclonedds applies for its localhost mode.
  if (std::getenv("CYCLONEDDS_URI") != nullptr) {
    return std::string();
  }
  if (!localhost_only) {
    return std::string();
  }
  return "<CycloneDDS><Domain>"
         "<General><AllowMulticast>false</AllowMulticast></General>"
         "<Discovery>"
         "<ParticipantIndex>auto</ParticipantIndex>"
         "<MaxAutoParticipantIndex>32</MaxAutoParticipantIndex>"
         "<Peers><Peer address=\"localhost\"/></Peers>"
         "</Discovery></Domain></CycloneDDS>";
}

dds_qos_t* make_dds_qos(const QoS& qos) {
  dds_qos_t* q = base_dds_qos();
  const dds_duration_t inf = DDS_INFINITY;
  dds_qset_history(q,
                   qos.history == QoS::History::KeepAll ? DDS_HISTORY_KEEP_ALL
                                                        : DDS_HISTORY_KEEP_LAST,
                   static_cast<int32_t>(qos.depth));
  dds_qset_reliability(q,
                       qos.reliability == QoS::Reliability::Reliable
                           ? DDS_RELIABILITY_RELIABLE
                           : DDS_RELIABILITY_BEST_EFFORT,
                       inf);
  dds_qset_durability(q, qos.durability == QoS::Durability::TransientLocal
                             ? DDS_DURABILITY_TRANSIENT_LOCAL
                             : DDS_DURABILITY_VOLATILE);
  dds_qset_deadline(q, inf);
  return q;
}

Participant::Participant(uint32_t domain_id, const std::string& enclave) {
  // Compose the domain configuration exactly like rmw_cyclonedds does:
  // an optional localhost fragment, then the user's CYCLONEDDS_URI (which
  // CycloneDDS does NOT pick up on its own when a non-NULL config is passed,
  // and which we must pass explicitly because we always call
  // dds_create_domain).  Fragments are comma-separated.
  std::string cfg = default_config_xml(Context::instance().localhost_only());
  if (const char* env = std::getenv("CYCLONEDDS_URI")) {
    if (!cfg.empty()) {
      cfg += ",";
    }
    cfg += env;
  }
  const char* cfg_ptr = cfg.empty() ? nullptr : cfg.c_str();
  const dds_entity_t dom =
      dds_create_domain(static_cast<dds_domainid_t>(domain_id), cfg_ptr);
  if (dom < 0 && dom != DDS_RETCODE_PRECONDITION_NOT_MET) {
    throw Error(vila::InternalError("dds_create_domain failed: {}", dom));
  }

  dds_qos_t* qos = base_dds_qos();
  const std::string user_data = "enclave=" + enclave + ";";
  dds_qset_userdata(qos, user_data.c_str(), user_data.size());

  pp_ = dds_create_participant(static_cast<dds_domainid_t>(domain_id), qos, nullptr);
  dds_delete_qos(qos);
  if (pp_ < 0) {
    throw Error(vila::InternalError("dds_create_participant failed: {}", pp_));
  }
  pub_ = dds_create_publisher(pp_, nullptr, nullptr);
  sub_ = dds_create_subscriber(pp_, nullptr, nullptr);
  if (pub_ < 0 || sub_ < 0) {
    throw Error(vila::InternalError("failed to create pub/sub entities"));
  }
}

Participant::~Participant() {
  if (pp_ >= 0) {
    dds_delete(pp_);
  }
}

Writer::Writer(dds_entity_t participant, dds_entity_t dds_publisher,
               const std::string& topic, const std::string& type_name, const QoS& qos) {
  sertype_ = create_raw_sertype(type_name.c_str());
  if (sertype_ == nullptr) {
    throw Error(vila::InternalError("failed to create sertype for {}", type_name));
  }
  dds_qos_t* q = make_dds_qos(qos);
  const dds_entity_t tp = dds_create_topic_sertype(
      participant, topic.c_str(), &sertype_, nullptr, nullptr, nullptr);
  if (tp < 0) {
    dds_delete_qos(q);
    throw Error(vila::InternalError("dds_create_topic_sertype failed for {}", topic));
  }
  wr_ = dds_create_writer(dds_publisher, tp, q, nullptr);
  dds_delete_qos(q);
  if (wr_ < 0) {
    throw Error(vila::InternalError("dds_create_writer failed for {}", topic));
  }
}

Writer::~Writer() {
  if (wr_ >= 0) {
    dds_delete(wr_);
  }
}

bool Writer::write(const uint8_t* payload, size_t size) {
  struct ddsi_serdata* d = raw_serdata_from_blob(sertype_, payload, size);
  if (d == nullptr) {
    return false;
  }
  // Ownership contract: dds_writecdr consumes one reference on all paths
  // through the write implementation (see dds_writecdr_impl_common in
  // CycloneDDS); the early validation failures it can return without
  // consuming cannot occur for our writers (valid handle, no content
  // filters).  Do NOT unref on failure — that would double-free.
  return dds_writecdr(wr_, d) == DDS_RETCODE_OK;
}

size_t Writer::reader_count() const {
  dds_publication_matched_status_t st;
  if (dds_get_publication_matched_status(wr_, &st) < 0) {
    return 0;
  }
  return static_cast<size_t>(st.current_count);
}

Reader::Reader(dds_entity_t participant, dds_entity_t dds_subscriber,
               const std::string& topic, const std::string& type_name, const QoS& qos) {
  struct ddsi_sertype* st = create_raw_sertype(type_name.c_str());
  if (st == nullptr) {
    throw Error(vila::InternalError("failed to create sertype for {}", type_name));
  }
  dds_qos_t* q = make_dds_qos(qos);
  const dds_entity_t tp = dds_create_topic_sertype(participant, topic.c_str(), &st,
                                                   nullptr, nullptr, nullptr);
  if (tp < 0) {
    dds_delete_qos(q);
    throw Error(vila::InternalError("dds_create_topic_sertype failed for {}", topic));
  }
  rd_ = dds_create_reader(dds_subscriber, tp, q, nullptr);
  dds_delete_qos(q);
  if (rd_ < 0) {
    throw Error(vila::InternalError("dds_create_reader failed for {}", topic));
  }
  rc_ = dds_create_readcondition(rd_, DDS_ANY_STATE);
  if (rc_ < 0) {
    throw Error(vila::InternalError("dds_create_readcondition failed for {}", topic));
  }
}

Reader::~Reader() {
  if (rc_ >= 0) {
    dds_delete(rc_);
  }
  if (rd_ >= 0) {
    dds_delete(rd_);
  }
}

size_t Reader::writer_count() const {
  dds_subscription_matched_status_t st;
  if (dds_get_subscription_matched_status(rd_, &st) < 0) {
    return 0;
  }
  return static_cast<size_t>(st.current_count);
}

GuardCondition::GuardCondition() {
  gcond_ = dds_create_guardcondition(DDS_CYCLONEDDS_HANDLE);
  if (gcond_ < 0) {
    throw Error(vila::InternalError("dds_create_guardcondition failed"));
  }
}

GuardCondition::~GuardCondition() {
  if (gcond_ >= 0) {
    dds_delete(gcond_);
  }
}

void GuardCondition::trigger() { dds_set_guardcondition(gcond_, true); }

bool GuardCondition::take() {
  bool triggered = false;
  dds_take_guardcondition(gcond_, &triggered);
  return triggered;
}

}  // namespace dds
}  // namespace rcl
