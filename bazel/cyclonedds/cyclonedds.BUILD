# BUILD file used INSIDE the CycloneDDS repository fetched via
# new_git_repository (see MODULE.bazel).  It only exports filegroups of the
# upstream sources; the actual compilation happens in //bazel/cyclonedds:ddsc
# so the platform selects and the generated headers stay in our tree.

load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

filegroup(
    name = "ddsc_common_srcs",
    srcs = glob(
        [
            "src/core/ddsc/src/*.c",
            "src/core/ddsi/src/*.c",
            "src/security/core/src/*.c",
            "src/ddsrt/src/*.c",  # top level only: platform code lives below
        ],
        exclude = [
            # iceoryx / SHM support is disabled (no DDS_HAS_SHM)
            "src/core/ddsc/src/shm_monitor.c",
            "src/core/ddsi/src/ddsi_shm_transport.c",
            # .part.c files are #included by their parent translation unit
            "src/core/ddsi/src/*.part.c",
        ],
    ) + ["src/core/ddsi/defconfig.c"],
)

# .part.c fragments #included by ddsi_cdrstream.c (declared as textual_hdrs
# by the consumer).
filegroup(
    name = "ddsc_part_fragments",
    srcs = glob(["src/core/ddsi/src/*.part.c"]),
)

filegroup(
    name = "public_headers",
    srcs = glob([
        "src/core/ddsc/include/**/*.h",
        "src/core/ddsc/src/*.h",
        "src/core/ddsi/include/**/*.h",
        "src/core/ddsi/src/*.h",
        "src/ddsrt/include/**/*.h",
        "src/ddsrt/src/*.h",
        "src/security/api/include/**/*.h",
        "src/security/core/include/**/*.h",
    ]),
)

# Platform-dependent ddsrt sources (mirrors src/ddsrt/CMakeLists.txt).
filegroup(
    name = "ddsrt_windows_srcs",
    srcs = [
        "src/ddsrt/src/dynlib/windows/dynlib.c",
        "src/ddsrt/src/environ/windows/environ.c",
        "src/ddsrt/src/filesystem/windows/filesystem.c",
        "src/ddsrt/src/heap/posix/heap.c",
        "src/ddsrt/src/ifaddrs/windows/ifaddrs.c",
        "src/ddsrt/src/netstat/windows/netstat.c",
        "src/ddsrt/src/process/windows/process.c",
        "src/ddsrt/src/random/windows/random.c",
        "src/ddsrt/src/rusage/windows/rusage.c",
        "src/ddsrt/src/sockets/windows/gethostname.c",
        "src/ddsrt/src/sockets/windows/socket.c",
        "src/ddsrt/src/sync/windows/sync.c",
        "src/ddsrt/src/threads/windows/threads.c",
        "src/ddsrt/src/time/windows/time.c",
    ],
)

filegroup(
    name = "ddsrt_posix_srcs",
    srcs = [
        "src/ddsrt/src/dynlib/posix/dynlib.c",
        "src/ddsrt/src/environ/posix/environ.c",
        "src/ddsrt/src/filesystem/posix/filesystem.c",
        "src/ddsrt/src/heap/posix/heap.c",
        "src/ddsrt/src/ifaddrs/posix/ifaddrs.c",
        "src/ddsrt/src/process/posix/process.c",
        "src/ddsrt/src/random/posix/random.c",
        "src/ddsrt/src/rusage/posix/rusage.c",
        "src/ddsrt/src/sockets/posix/gethostname.c",
        "src/ddsrt/src/sockets/posix/socket.c",
        "src/ddsrt/src/sync/posix/sync.c",
        "src/ddsrt/src/threads/posix/threads.c",
    ],
)

filegroup(
    name = "ddsrt_linux_srcs",
    srcs = [
        "src/ddsrt/src/netstat/linux/netstat.c",
        "src/ddsrt/src/time/posix/time.c",
    ],
)

filegroup(
    name = "ddsrt_macos_srcs",
    srcs = [
        "src/ddsrt/src/netstat/darwin/netstat.c",
        "src/ddsrt/src/time/darwin/time.c",
    ],
)

filegroup(
    name = "ddsrt_generic_srcs",
    srcs = [
        "src/ddsrt/src/time/posix/time.c",
    ],
)

# Header-only library exporting the include paths into the fetched tree;
# consumed by //bazel/cyclonedds:ddsc and by anything needing the ddsc
# headers without the library (e.g. the mock ddsc in tests).
cc_library(
    name = "headers",
    hdrs = [":public_headers"],
    includes = [
        "src/core/ddsc/include",
        "src/core/ddsc/src",
        "src/core/ddsi/include",
        "src/ddsrt/include",
        "src/ddsrt/src",
        "src/security/api/include",
        "src/security/core/include",
    ],
)
