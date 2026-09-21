// Copyright 2026 rcllite contributors
// Licensed under the Apache License, Version 2.0
#include "rcllite/dds/raw_serdata.hpp"

#include <cstdio>
#include <cstring>
#include <functional>
#include <new>
#include <string>

#include "dds/ddsi/q_radmin.h"

namespace rcl {
namespace dds {
namespace {

RawSerdata* as_raw(struct ddsi_serdata* d) { return reinterpret_cast<RawSerdata*>(d); }
const RawSerdata* as_raw(const struct ddsi_serdata* d) {
  return reinterpret_cast<const RawSerdata*>(d);
}

void raw_resize(RawSerdata* d, size_t requested) {
  // CycloneDDS' network path may read up to the next 4-byte boundary; pad
  // the buffer so those reads stay in bounds.
  const size_t pad = (0u - static_cast<unsigned int>(requested)) % 4u;
  d->payload.assign(requested + pad, 0);
}

bool raw_eqkey(const struct ddsi_serdata*, const struct ddsi_serdata*) {
  // ROS 2 messages never declare DDS keys.
  return true;
}

uint32_t raw_size(const struct ddsi_serdata* dcmn) {
  return static_cast<uint32_t>(as_raw(dcmn)->payload.size());
}

struct ddsi_serdata* raw_from_ser(const struct ddsi_sertype* type,
                                  enum ddsi_serdata_kind kind,
                                  const struct nn_rdata* fragchain, size_t size) {
  auto* d = new (std::nothrow) RawSerdata();
  if (d == nullptr) {
    return nullptr;
  }
  ddsi_serdata_init(&d->c, type, kind);
  try {
    raw_resize(d, size);
  } catch (...) {
    delete d;
    return nullptr;
  }

  // Reassemble the sample from the fragment chain.  Mirrors
  // rmw_cyclonedds' serdata_rmw_from_ser: the payload of each fragment lives
  // at NN_RDATA_PAYLOAD_OFF(fragchain) bytes into its rmsg, covering bytes
  // [min, maxp1) of the serialized sample.
  uint32_t off = 0;
  uint8_t* cursor = d->payload.data();
  while (fragchain) {
    if (fragchain->maxp1 > off) {
      const unsigned char* payload = static_cast<const unsigned char*>(
          NN_RMSG_PAYLOADOFF(fragchain->rmsg, NN_RDATA_PAYLOAD_OFF(fragchain)));
      const auto* src = payload + off - fragchain->min;
      const auto n_bytes = static_cast<size_t>(fragchain->maxp1 - off);
      std::memcpy(cursor, src, n_bytes);
      cursor += n_bytes;
      off = fragchain->maxp1;
    }
    fragchain = fragchain->nextfrag;
  }
  return &d->c;
}

struct ddsi_serdata* raw_from_ser_iov(const struct ddsi_sertype* type,
                                      enum ddsi_serdata_kind kind,
                                      ddsrt_msg_iovlen_t niov, const ddsrt_iovec_t* iov,
                                      size_t size) {
  auto* d = new (std::nothrow) RawSerdata();
  if (d == nullptr) {
    return nullptr;
  }
  ddsi_serdata_init(&d->c, type, kind);
  try {
    raw_resize(d, size);
  } catch (...) {
    delete d;
    return nullptr;
  }
  uint8_t* cursor = d->payload.data();
  for (ddsrt_msg_iovlen_t i = 0; i < niov; ++i) {
    std::memcpy(cursor, iov[i].iov_base, iov[i].iov_len);
    cursor += iov[i].iov_len;
  }
  return &d->c;
}

struct ddsi_serdata* raw_from_keyhash(const struct ddsi_sertype*,
                                      const struct ddsi_keyhash*) {
  return nullptr;  // no keys
}

struct ddsi_serdata* raw_from_sample(const struct ddsi_sertype*, enum ddsi_serdata_kind,
                                     const void*) {
  return nullptr;  // rcllite never writes typed samples
}

void raw_to_ser(const struct ddsi_serdata* dcmn, size_t off, size_t sz, void* buf) {
  std::memcpy(buf, as_raw(dcmn)->payload.data() + off, sz);
}

struct ddsi_serdata* raw_to_ser_ref(const struct ddsi_serdata* dcmn, size_t off,
                                    size_t sz, ddsrt_iovec_t* ref) {
  ref->iov_base = const_cast<uint8_t*>(as_raw(dcmn)->payload.data()) + off;
  ref->iov_len = static_cast<ddsrt_iov_len_t>(sz);
  return ddsi_serdata_ref(dcmn);
}

void raw_to_ser_unref(struct ddsi_serdata* dcmn, const ddsrt_iovec_t*) {
  ddsi_serdata_unref(dcmn);
}

bool raw_to_sample(const struct ddsi_serdata*, void*, void**, void*) {
  return false;  // samples are only consumed as raw CDR (dds_takecdr)
}

struct ddsi_serdata* raw_to_untyped(const struct ddsi_serdata* dcmn) {
  auto* src = as_raw(dcmn);
  auto* d = new (std::nothrow) RawSerdata();
  if (d == nullptr) {
    return nullptr;
  }
  ddsi_serdata_init(&d->c, src->c.type, SDK_KEY);
  d->c.type = nullptr;
  return &d->c;
}

bool raw_untyped_to_sample(const struct ddsi_sertype*, const struct ddsi_serdata*,
                           void*, void**, void*) {
  return false;
}

void raw_free(struct ddsi_serdata* dcmn) { delete as_raw(dcmn); }

size_t raw_print(const struct ddsi_sertype*, const struct ddsi_serdata* dcmn, char* buf,
                 size_t size) {
  auto* d = as_raw(dcmn);
  size_t n =
      static_cast<size_t>(std::snprintf(buf, size, "[opq %zu b]", d->payload.size()));
  return n < size ? n : size;
}

void raw_get_keyhash(const struct ddsi_serdata*, struct ddsi_keyhash* buf, bool) {
  std::memset(buf, 0, sizeof(*buf));
}

const struct ddsi_serdata_ops raw_serdata_ops = {
    raw_eqkey,              // eqkey
    raw_size,               // size
    raw_from_ser,           // from_ser
    raw_from_ser_iov,       // from_ser_iov
    raw_from_keyhash,       // from_keyhash
    raw_from_sample,        // from_sample
    raw_to_ser,             // to_ser
    raw_to_ser_ref,         // to_ser_ref
    raw_to_ser_unref,       // to_ser_unref
    raw_to_sample,          // to_sample
    raw_to_untyped,         // to_untyped
    raw_untyped_to_sample,  // untyped_to_sample
    raw_free,               // free
    raw_print,              // print
    raw_get_keyhash         // get_keyhash
};

void sertype_raw_free(struct ddsi_sertype* tpcmn) {
  ddsi_sertype_fini(tpcmn);
  delete tpcmn;
}

void sertype_raw_zero_samples(const struct ddsi_sertype*, void*, size_t) {}

void sertype_raw_realloc_samples(void**, const struct ddsi_sertype*, void*, size_t,
                                 size_t) {
  // Only used by dispose/unregister/loans, none of which rcllite exercises.
}

void sertype_raw_free_samples(const struct ddsi_sertype*, void**, size_t,
                              dds_free_op_t) {}

bool sertype_raw_equal(const struct ddsi_sertype* a, const struct ddsi_sertype* b) {
  return std::strcmp(a->type_name, b->type_name) == 0;
}

uint32_t sertype_raw_hash(const struct ddsi_sertype* tp) {
  return static_cast<uint32_t>(std::hash<std::string>{}(tp->type_name));
}

size_t sertype_raw_get_serialized_size(const struct ddsi_sertype*, const void*) {
  return 0;
}

bool sertype_raw_serialize_into(const struct ddsi_sertype*, const void*, void*,
                                size_t) {
  return false;
}

const struct ddsi_sertype_ops raw_sertype_ops = {
    ddsi_sertype_v0,                  // version
    nullptr,                          // arg
    sertype_raw_free,                 // free
    sertype_raw_zero_samples,         // zero_samples
    sertype_raw_realloc_samples,      // realloc_samples
    sertype_raw_free_samples,         // free_samples
    sertype_raw_equal,                // equal
    sertype_raw_hash,                 // hash
    nullptr,                          // type_id
    nullptr,                          // type_map
    nullptr,                          // type_info
    nullptr,                          // derive_sertype
    sertype_raw_get_serialized_size,  // get_serialized_size
    sertype_raw_serialize_into        // serialize_into
};

}  // namespace

struct ddsi_sertype* create_raw_sertype(const char* type_name) {
  auto* st = new (std::nothrow) struct ddsi_sertype();
  if (st == nullptr) {
    return nullptr;
  }
  ddsi_sertype_init_flags(st, type_name, &raw_sertype_ops, &raw_serdata_ops,
                          DDSI_SERTYPE_FLAG_TOPICKIND_NO_KEY);
  st->allowed_data_representation = DDS_DATA_REPRESENTATION_FLAG_XCDR1;
  return st;
}

struct ddsi_serdata* raw_serdata_from_blob(const struct ddsi_sertype* type,
                                           const void* blob, size_t size) {
  ddsrt_iovec_t iov;
  iov.iov_base = const_cast<void*>(blob);
  if (static_cast<size_t>(static_cast<ddsrt_iov_len_t>(size)) != size) {
    return nullptr;
  }
  iov.iov_len = static_cast<ddsrt_iov_len_t>(size);
  return raw_from_ser_iov(type, SDK_DATA, 1, &iov, size);
}

const uint8_t* raw_serdata_payload(const struct ddsi_serdata* d) {
  return as_raw(d)->payload.data();
}

size_t raw_serdata_payload_size(const struct ddsi_serdata* d) {
  return as_raw(d)->payload.size();
}

}  // namespace dds
}  // namespace rcl
