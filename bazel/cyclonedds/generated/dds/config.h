/*
 * Stand-in for the CMake-generated dds/config.h.
 * Platform feature detection mirrors the cmake checks of CycloneDDS 0.10.4
 * for the platforms rcllite supports (Windows, Linux, macOS, generic POSIX).
 */
#ifndef DDS_CONFIG_H
#define DDS_CONFIG_H

#if defined(_WIN32)

#define DDSRT_HAVE_DYNLIB 1
#define DDSRT_HAVE_FILESYSTEM 1
#define DDSRT_HAVE_NETSTAT 1
#define DDSRT_HAVE_RUSAGE 1
#define DDSRT_HAVE_IPV6 1
#define DDSRT_HAVE_DNS 1
#define DDSRT_HAVE_GETADDRINFO 1
#define DDSRT_HAVE_GETHOSTNAME 1
#define DDSRT_HAVE_INET_NTOP 1
#define DDSRT_HAVE_INET_PTON 1

#elif defined(__linux__) || defined(__APPLE__)

#define DDSRT_HAVE_DYNLIB 1
#define DDSRT_HAVE_FILESYSTEM 1
#define DDSRT_HAVE_NETSTAT 1
#define DDSRT_HAVE_RUSAGE 1
#define DDSRT_HAVE_IPV6 1
#define DDSRT_HAVE_DNS 1
#define DDSRT_HAVE_GETADDRINFO 1
#if defined(__linux__)
#define DDSRT_HAVE_GETHOSTBYNAME_R 1
#endif
#define DDSRT_HAVE_GETHOSTNAME 1
#define DDSRT_HAVE_INET_NTOP 1
#define DDSRT_HAVE_INET_PTON 1

#else /* generic POSIX fallback */

#define DDSRT_HAVE_DYNLIB 1
#define DDSRT_HAVE_FILESYSTEM 1
#define DDSRT_HAVE_IPV6 1
#define DDSRT_HAVE_DNS 1
#define DDSRT_HAVE_GETADDRINFO 1
#define DDSRT_HAVE_GETHOSTNAME 1
#define DDSRT_HAVE_INET_NTOP 1
#define DDSRT_HAVE_INET_PTON 1

#endif

#endif
