// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
//
// Mock implementation of the CycloneDDS C API subset used by rcllite.
// See mock_ddsc.h for the test-side control API.
//
// Semantics mirrored from CycloneDDS 0.10.4 where rcllite depends on them:
//   * dds_writecdr consumes one serdata reference on every path;
//   * ddsi_serdata_init leaves refc == 1, unref frees via ops->free;
//   * ddsi_sertype_init_flags copies the type name (freed in fini);
//   * waitsets report attachments whose guard flag is set or whose
//     read-condition reader has queued samples.

#include "mock/mock_ddsc.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "dds/ddsi/ddsi_serdata.h"
#include "dds/ddsi/ddsi_sertype.h"
#include "dds/ddsrt/atomics.h"

namespace mock_dds {
namespace internal {

// Layout-compatible with rcl::dds::RawSerdata so the reinterpret_casts
// inside rcllite's take/write paths stay valid.
struct MockSerdata {
  struct ddsi_serdata c;
  std::vector<uint8_t> payload;
};

struct QosState {
  dds_history_kind_t history = DDS_HISTORY_KEEP_LAST;
  int32_t depth = 10;
  dds_reliability_kind_t reliability = DDS_RELIABILITY_RELIABLE;
  dds_durability_kind_t durability = DDS_DURABILITY_VOLATILE;
};

struct ReaderState {
  std::vector<std::vector<uint8_t>> queue;
  int32_t matched = 0;
  dds_entity_t readcond = 0;
  const struct ddsi_sertype* sertype = nullptr;
  QosView qos{};
};

struct WriterState {
  std::vector<std::vector<uint8_t>> written;
  QosView qos{};
};

struct GuardState {
  std::atomic<bool> flag{false};
};

struct WaitsetState {
  std::map<dds_entity_t, dds_attach_t> attached;  // cond -> attachment id
};

std::mutex mtx;
// dds_qos_t stays opaque (the ddsi headers define the real struct); the mock
// keeps the QoS values set through the q setters in a pointer-keyed map.
std::map<const dds_qos_t*, QosState> qos_state;
std::map<dds_entity_t, ReaderState> readers;
std::map<dds_entity_t, WriterState> writers;
std::map<dds_entity_t, GuardState> guards;
std::map<dds_entity_t, WaitsetState> waitsets;
std::map<dds_entity_t, const struct ddsi_sertype*> topic_sertypes;
dds_entity_t next_handle = 100;
dds_return_t write_result = DDS_RETCODE_OK;
dds_entity_t last_reader_h = 0;
dds_entity_t last_writer_h = 0;
dds_entity_t last_readcond_h = 0;

dds_entity_t alloc_handle() { return next_handle++; }

QosView snapshot(const QosState& s) {
  return QosView{s.history, s.depth, s.reliability, s.durability};
}

// Reader owning a read-condition (conditions are created per reader).
dds_entity_t reader_of_cond(dds_entity_t cond) {
  for (const auto& kv : readers) {
    if (kv.second.readcond == cond) {
      return kv.first;
    }
  }
  return 0;
}

}  // namespace internal

// --- test-side control API ---------------------------------------------------

void reset() {
  std::lock_guard<std::mutex> lock(internal::mtx);
  internal::qos_state.clear();
  internal::readers.clear();
  internal::writers.clear();
  internal::guards.clear();
  internal::waitsets.clear();
  internal::topic_sertypes.clear();
  internal::next_handle = 100;
  internal::write_result = DDS_RETCODE_OK;
  internal::last_reader_h = 0;
  internal::last_writer_h = 0;
  internal::last_readcond_h = 0;
}

dds_entity_t last_reader() { return internal::last_reader_h; }

dds_entity_t last_writer() { return internal::last_writer_h; }

dds_entity_t last_readcondition() { return internal::last_readcond_h; }

void enqueue_sample(dds_entity_t reader, const void* data, size_t size) {
  std::lock_guard<std::mutex> lock(internal::mtx);
  const auto* p = static_cast<const uint8_t*>(data);
  internal::readers[reader].queue.emplace_back(p, p + size);
}

const std::vector<std::vector<uint8_t>>& written(dds_entity_t writer) {
  return internal::writers[writer].written;
}

void set_write_result(dds_return_t rc) {
  std::lock_guard<std::mutex> lock(internal::mtx);
  internal::write_result = rc;
}

void set_matched_writers(dds_entity_t reader, int32_t count) {
  std::lock_guard<std::mutex> lock(internal::mtx);
  internal::readers[reader].matched = count;
}

const QosView* writer_qos(dds_entity_t writer) {
  auto it = internal::writers.find(writer);
  return it == internal::writers.end() ? nullptr : &it->second.qos;
}

const QosView* reader_qos(dds_entity_t reader) {
  auto it = internal::readers.find(reader);
  return it == internal::readers.end() ? nullptr : &it->second.qos;
}

}  // namespace mock_dds

extern "C" {

// ---------------------------------------------------------------------------
// ddsc API
// ---------------------------------------------------------------------------

// The CycloneDDS prototypes qualify qos parameters __restrict, and gcc keeps
// that in the pointer's type — such a value cannot bind to std::map's
// reference parameters.  Passing it by value through a plain-pointer
// parameter strips the qualifier (MSVC ignores it either way).
inline const dds_qos_t* qos_key(const dds_qos_t* qos) { return qos; }

dds_qos_t* dds_create_qos(void) {
  return new dds_qos_t{};  // opaque handle; state lives in the map below
}

void dds_delete_qos(dds_qos_t* __restrict qos) {
  std::lock_guard<std::mutex> lock(mock_dds::internal::mtx);
  mock_dds::internal::qos_state.erase(qos_key(qos));
  delete qos;
}

void dds_qset_history(dds_qos_t* __restrict qos, dds_history_kind_t kind,
                      int32_t depth) {
  std::lock_guard<std::mutex> lock(mock_dds::internal::mtx);
  auto& s = mock_dds::internal::qos_state[qos_key(qos)];
  s.history = kind;
  s.depth = depth;
}

void dds_qset_reliability(dds_qos_t* __restrict qos, dds_reliability_kind_t kind,
                          dds_duration_t max_blocking_time) {
  (void)max_blocking_time;
  std::lock_guard<std::mutex> lock(mock_dds::internal::mtx);
  mock_dds::internal::qos_state[qos_key(qos)].reliability = kind;
}

void dds_qset_durability(dds_qos_t* __restrict qos, dds_durability_kind_t kind) {
  std::lock_guard<std::mutex> lock(mock_dds::internal::mtx);
  mock_dds::internal::qos_state[qos_key(qos)].durability = kind;
}

void dds_qset_deadline(dds_qos_t* __restrict qos, dds_duration_t period) {
  (void)qos;
  (void)period;
}

void dds_qset_userdata(dds_qos_t* __restrict qos, const void* __restrict value,
                       size_t size) {
  (void)qos;
  (void)value;
  (void)size;
}

void dds_free(void* ptr) { std::free(ptr); }

bool dds_qget_userdata(const dds_qos_t* __restrict qos, void** value, size_t* sz) {
  (void)qos;
  (void)value;
  (void)sz;
  return false;  // userdata is never set on mock qos objects
}

dds_entity_t dds_create_domain(dds_domainid_t domain, const char* config) {
  (void)domain;
  (void)config;
  return mock_dds::internal::alloc_handle();
}

dds_entity_t dds_create_participant(dds_domainid_t domain, const dds_qos_t* qos,
                                    const dds_listener_t* listener) {
  (void)domain;
  (void)qos;
  (void)listener;
  return mock_dds::internal::alloc_handle();
}

dds_entity_t dds_create_publisher(dds_entity_t participant, const dds_qos_t* qos,
                                  const dds_listener_t* listener) {
  (void)participant;
  (void)qos;
  (void)listener;
  return mock_dds::internal::alloc_handle();
}

dds_entity_t dds_create_subscriber(dds_entity_t participant, const dds_qos_t* qos,
                                   const dds_listener_t* listener) {
  (void)participant;
  (void)qos;
  (void)listener;
  return mock_dds::internal::alloc_handle();
}

dds_entity_t dds_create_topic_sertype(dds_entity_t participant, const char* name,
                                      struct ddsi_sertype** sertype,
                                      const dds_qos_t* qos,
                                      const dds_listener_t* listener,
                                      const struct ddsi_plist* sedp_plist) {
  (void)participant;
  (void)name;
  (void)qos;
  (void)listener;
  (void)sedp_plist;
  const dds_entity_t topic = mock_dds::internal::alloc_handle();
  std::lock_guard<std::mutex> lock(mock_dds::internal::mtx);
  mock_dds::internal::topic_sertypes[topic] = *sertype;
  return topic;
}

dds_entity_t dds_create_writer(dds_entity_t publisher, dds_entity_t topic,
                               const dds_qos_t* qos, const dds_listener_t* listener) {
  (void)publisher;
  (void)topic;
  (void)listener;
  const dds_entity_t writer = mock_dds::internal::alloc_handle();
  std::lock_guard<std::mutex> lock(mock_dds::internal::mtx);
  auto& st = mock_dds::internal::writers[writer];
  auto qIt = mock_dds::internal::qos_state.find(qos);
  st.qos = qIt != mock_dds::internal::qos_state.end()
               ? mock_dds::internal::snapshot(qIt->second)
               : mock_dds::internal::snapshot(mock_dds::internal::QosState{});
  mock_dds::internal::last_writer_h = writer;
  return writer;
}

dds_entity_t dds_create_reader(dds_entity_t subscriber, dds_entity_t topic,
                               const dds_qos_t* qos, const dds_listener_t* listener) {
  (void)subscriber;
  (void)listener;
  const dds_entity_t reader = mock_dds::internal::alloc_handle();
  std::lock_guard<std::mutex> lock(mock_dds::internal::mtx);
  auto& st = mock_dds::internal::readers[reader];
  auto qIt = mock_dds::internal::qos_state.find(qos);
  st.qos = qIt != mock_dds::internal::qos_state.end()
               ? mock_dds::internal::snapshot(qIt->second)
               : mock_dds::internal::snapshot(mock_dds::internal::QosState{});
  auto tIt = mock_dds::internal::topic_sertypes.find(topic);
  st.sertype = tIt != mock_dds::internal::topic_sertypes.end() ? tIt->second : nullptr;
  mock_dds::internal::last_reader_h = reader;
  return reader;
}

dds_entity_t dds_create_readcondition(dds_entity_t reader, uint32_t mask) {
  (void)mask;
  const dds_entity_t cond = mock_dds::internal::alloc_handle();
  std::lock_guard<std::mutex> lock(mock_dds::internal::mtx);
  mock_dds::internal::readers[reader].readcond = cond;
  mock_dds::internal::last_readcond_h = cond;
  return cond;
}

dds_entity_t dds_create_guardcondition(dds_entity_t parent) {
  (void)parent;
  const dds_entity_t guard = mock_dds::internal::alloc_handle();
  std::lock_guard<std::mutex> lock(mock_dds::internal::mtx);
  mock_dds::internal::guards[guard];  // default flag = false
  return guard;
}

dds_return_t dds_set_guardcondition(dds_entity_t guardcond, bool triggered) {
  std::lock_guard<std::mutex> lock(mock_dds::internal::mtx);
  auto it = mock_dds::internal::guards.find(guardcond);
  if (it == mock_dds::internal::guards.end()) {
    return DDS_RETCODE_BAD_PARAMETER;
  }
  it->second.flag.store(triggered, std::memory_order_release);
  return DDS_RETCODE_OK;
}

dds_return_t dds_take_guardcondition(dds_entity_t guardcond, bool* triggered) {
  std::lock_guard<std::mutex> lock(mock_dds::internal::mtx);
  auto it = mock_dds::internal::guards.find(guardcond);
  if (it == mock_dds::internal::guards.end()) {
    *triggered = false;
    return DDS_RETCODE_BAD_PARAMETER;
  }
  *triggered = it->second.flag.exchange(false, std::memory_order_acq_rel);
  return DDS_RETCODE_OK;
}

dds_return_t dds_delete(dds_entity_t entity) {
  std::lock_guard<std::mutex> lock(mock_dds::internal::mtx);
  mock_dds::internal::readers.erase(entity);
  mock_dds::internal::writers.erase(entity);
  mock_dds::internal::guards.erase(entity);
  mock_dds::internal::waitsets.erase(entity);
  mock_dds::internal::topic_sertypes.erase(entity);
  return DDS_RETCODE_OK;
}

dds_return_t dds_get_guid(dds_entity_t entity, dds_guid_t* guid) {
  (void)entity;
  for (int i = 0; i < 16; ++i) {
    guid->v[i] = static_cast<unsigned char>(i + 1);
  }
  return DDS_RETCODE_OK;
}

dds_return_t dds_get_subscription_matched_status(
    dds_entity_t reader, dds_subscription_matched_status_t* status) {
  std::lock_guard<std::mutex> lock(mock_dds::internal::mtx);
  auto it = mock_dds::internal::readers.find(reader);
  status->current_count =
      it != mock_dds::internal::readers.end() ? it->second.matched : 0;
  status->current_count_change = 0;
  status->total_count = status->current_count;
  status->total_count_change = 0;
  return DDS_RETCODE_OK;
}

dds_return_t dds_writecdr(dds_entity_t writer, struct ddsi_serdata* serdata) {
  dds_return_t rc;
  {
    std::lock_guard<std::mutex> lock(mock_dds::internal::mtx);
    rc = mock_dds::internal::write_result;
    if (rc == DDS_RETCODE_OK) {
      const auto* d = reinterpret_cast<const mock_dds::internal::MockSerdata*>(serdata);
      mock_dds::internal::writers[writer].written.push_back(d->payload);
    }
  }
  ddsi_serdata_unref(serdata);  // consumes one reference on every path
  return rc;
}

dds_return_t dds_takecdr(dds_entity_t reader, struct ddsi_serdata** buf, uint32_t maxs,
                         dds_sample_info_t* si, uint32_t mask) {
  (void)mask;
  std::vector<std::vector<uint8_t>> drained;
  const struct ddsi_sertype* sertype = nullptr;
  {
    std::lock_guard<std::mutex> lock(mock_dds::internal::mtx);
    auto it = mock_dds::internal::readers.find(reader);
    if (it == mock_dds::internal::readers.end()) {
      return DDS_RETCODE_BAD_PARAMETER;
    }
    auto& q = it->second.queue;
    const size_t n = q.size() < maxs ? q.size() : static_cast<size_t>(maxs);
    drained.assign(q.begin(), q.begin() + n);
    q.erase(q.begin(), q.begin() + n);
    sertype = it->second.sertype;
  }
  for (size_t i = 0; i < drained.size(); ++i) {
    auto* d = new mock_dds::internal::MockSerdata();
    ddsi_serdata_init(&d->c, sertype, SDK_DATA);
    d->payload = std::move(drained[i]);
    buf[i] = &d->c;
    si[i] = dds_sample_info_t{};
    si[i].valid_data = true;
  }
  return static_cast<dds_return_t>(drained.size());
}

// Typed-sample API used by GraphMonitor's builtin-topic readers (rt/rosout,
// ros_discovery_info).  The mock simulates no builtin traffic, so take
// always reports an empty batch; rcllite's data path goes through
// dds_takecdr above and is covered by the tests.
dds_return_t dds_take(dds_entity_t reader_or_condition, void** buf,
                      dds_sample_info_t* si, size_t bufsz, uint32_t maxs) {
  (void)reader_or_condition;
  (void)buf;
  (void)si;
  (void)bufsz;
  (void)maxs;
  return 0;
}

dds_return_t dds_return_loan(dds_entity_t entity, void** buf, int32_t bufsz) {
  (void)entity;
  if (buf != nullptr && bufsz > 0) {
    std::memset(buf, 0, static_cast<size_t>(bufsz) * sizeof(*buf));
  }
  return DDS_RETCODE_OK;
}

dds_entity_t dds_create_waitset(dds_entity_t parent) {
  (void)parent;
  const dds_entity_t ws = mock_dds::internal::alloc_handle();
  std::lock_guard<std::mutex> lock(mock_dds::internal::mtx);
  mock_dds::internal::waitsets[ws];  // default-construct
  return ws;
}

dds_return_t dds_waitset_attach(dds_entity_t waitset, dds_entity_t entity,
                                dds_attach_t x) {
  std::lock_guard<std::mutex> lock(mock_dds::internal::mtx);
  mock_dds::internal::waitsets[waitset].attached[entity] = x;
  return DDS_RETCODE_OK;
}

dds_return_t dds_waitset_detach(dds_entity_t waitset, dds_entity_t entity) {
  std::lock_guard<std::mutex> lock(mock_dds::internal::mtx);
  mock_dds::internal::waitsets[waitset].attached.erase(entity);
  return DDS_RETCODE_OK;
}

dds_return_t dds_waitset_wait(dds_entity_t waitset, dds_attach_t* xs, size_t nxs,
                              dds_duration_t reltimeout) {
  (void)reltimeout;  // deterministic, non-blocking: report what is ready now
  std::lock_guard<std::mutex> lock(mock_dds::internal::mtx);
  auto wIt = mock_dds::internal::waitsets.find(waitset);
  if (wIt == mock_dds::internal::waitsets.end()) {
    return DDS_RETCODE_BAD_PARAMETER;
  }
  size_t n = 0;
  for (const auto& kv : wIt->second.attached) {
    if (n == nxs) {
      break;
    }
    const dds_entity_t cond = kv.first;
    bool ready = false;
    auto gIt = mock_dds::internal::guards.find(cond);
    if (gIt != mock_dds::internal::guards.end()) {
      ready = gIt->second.flag.load(std::memory_order_acquire);
    } else {
      const dds_entity_t rd = mock_dds::internal::reader_of_cond(cond);
      ready = rd != 0 && !mock_dds::internal::readers[rd].queue.empty();
    }
    if (ready) {
      xs[n++] = kv.second;
    }
  }
  return static_cast<dds_return_t>(n);
}

// ---------------------------------------------------------------------------
// ddsi helpers (exported, non-inline)
// ---------------------------------------------------------------------------

// On Windows ddsi_sertype.h defines ddsi_sertype_v0 as a macro (CycloneDDS'
// own workaround), so the function only exists as a symbol elsewhere.
#ifndef _WIN32
void ddsi_sertype_v0(struct ddsi_sertype_v0* dummy) { (void)dummy; }
#endif

void ddsi_serdata_init(struct ddsi_serdata* d, const struct ddsi_sertype* tp,
                       enum ddsi_serdata_kind kind) {
  d->type = tp;
  d->ops = tp->serdata_ops;
  d->kind = kind;
  d->hash = 0;
  d->statusinfo = 0;
  d->timestamp.v = INT64_MIN;
  d->twrite.v = INT64_MIN;
  ddsrt_atomic_st32(&d->refc, 1);
}

void ddsi_sertype_init_flags(struct ddsi_sertype* tp, const char* type_name,
                             const struct ddsi_sertype_ops* sertype_ops,
                             const struct ddsi_serdata_ops* serdata_ops,
                             uint32_t flags) {
  (void)flags;
  ddsrt_atomic_st32(&tp->flags_refc, 1);
  char* name = new char[std::strlen(type_name) + 1];
  std::strcpy(name, type_name);
  tp->type_name = name;
  tp->ops = sertype_ops;
  tp->serdata_ops = serdata_ops;
  tp->serdata_basehash = 0;
  tp->typekind_no_key = 1u;
  tp->request_keyhash = 0u;
}

void ddsi_sertype_fini(struct ddsi_sertype* tp) {
  delete[] const_cast<char*>(tp->type_name);
}

}  // extern "C"
