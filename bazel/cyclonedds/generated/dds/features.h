/*
 * Stand-in for the CMake-generated dds/features.h.
 * Feature set mirrors the CycloneDDS 0.10.4 cmake defaults: security,
 * lifespan, deadline missed, network partitions, source-specific multicast,
 * type discovery and topic discovery enabled; SSL and iceoryx SHM disabled.
 */
#ifndef _DDS_PUBLIC_FEATURES_H_
#define _DDS_PUBLIC_FEATURES_H_

/* Whether or not support for DDS Security is included */
#define DDS_HAS_SECURITY 1

/* Whether or not support for the lifespan QoS is included */
#define DDS_HAS_LIFESPAN 1

/* Whether or not support for generating "deadline missed" events is included */
#define DDS_HAS_DEADLINE_MISSED 1

/* Whether or not support for network partitions is included */
#define DDS_HAS_NETWORK_PARTITIONS 1

/* Whether or not support for source-specific multicast is included */
#define DDS_HAS_SSM 1

/* Whether or not features dependent on OpenSSL are included */
/* #undef DDS_HAS_SSL */

/* Whether or not support for type discovery is included */
#define DDS_HAS_TYPE_DISCOVERY 1

/* Whether or not support for topic discovery is included */
#define DDS_HAS_TOPIC_DISCOVERY 1

/* Whether or not support for Iceoryx support is included */
/* #undef DDS_HAS_SHM */

#endif
