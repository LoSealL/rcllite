"""Shared compile/link option definitions for every rcllite target.

Two compiler families are supported:

  * Windows MSVC (cl.exe)   -> WINDOWS_MSVC_*
  * GCC / Clang (Linux/macOS) -> GCC_CLANG_*

The RCLLITE_DEFAULT_* selects pick the right family at analysis time, and the
rcllite_cc_* wrappers apply them so individual BUILD files stay terse.
"""

load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# --- Windows (MSVC) ----------------------------------------------------------

WINDOWS_MSVC_COPTS = [
    "/W4",  # high warning level
    "/utf-8",  # source + execution charset (required by fmt)
    "/bigobj",  # template-heavy translation units
    "/Zc:__cplusplus",
    "/permissive-",  # standards-conformant mode
    "/wd4389",  # signed/unsigned mismatch in CycloneDDS QoS macros
    "/wd4200",  # zero-sized array in struct (ddsi_serdata_default.h)
]

WINDOWS_MSVC_DEFINES = [
    "_CRT_SECURE_NO_WARNINGS",
]

WINDOWS_MSVC_LINKOPTS = [
    "ws2_32.lib",  # Winsock (CycloneDDS transport)
]

# --- GCC / Clang --------------------------------------------------------------

GCC_CLANG_COPTS = [
    "-Wall",
    "-Wextra",
    "-Wpedantic",
]

GCC_CLANG_DEFINES = []

GCC_CLANG_LINKOPTS = []

# --- Platform selects ----------------------------------------------------------

RCLLITE_DEFAULT_COPTS = select({
    "@platforms//os:windows": WINDOWS_MSVC_COPTS,
    "//conditions:default": GCC_CLANG_COPTS,
})

RCLLITE_DEFAULT_DEFINES = select({
    "@platforms//os:windows": WINDOWS_MSVC_DEFINES,
    "//conditions:default": GCC_CLANG_DEFINES,
})

RCLLITE_DEFAULT_LINKOPTS = select({
    "@platforms//os:windows": WINDOWS_MSVC_LINKOPTS,
    "//conditions:default": GCC_CLANG_LINKOPTS,
})

# --- Convenience wrappers ------------------------------------------------------

def rcllite_cc_library(name, **kwargs):
    """cc_library with the rcllite default copts/defines applied.

    Args:
      name: target name, passed through.
      **kwargs: any other cc_library attributes; user copts/defines are
        prepended to the rcllite defaults.
    """
    kwargs["copts"] = RCLLITE_DEFAULT_COPTS + kwargs.get("copts", [])
    kwargs["defines"] = RCLLITE_DEFAULT_DEFINES + kwargs.get("defines", [])
    cc_library(
        name = name,
        **kwargs
    )

def rcllite_cc_binary(name, **kwargs):
    """cc_binary with the rcllite default copts/defines/linkopts applied.

    Args:
      name: target name, passed through.
      **kwargs: any other cc_binary attributes; user copts/defines/linkopts
        are prepended to the rcllite defaults.
    """
    kwargs["copts"] = RCLLITE_DEFAULT_COPTS + kwargs.get("copts", [])
    kwargs["defines"] = RCLLITE_DEFAULT_DEFINES + kwargs.get("defines", [])
    kwargs["linkopts"] = RCLLITE_DEFAULT_LINKOPTS + kwargs.get("linkopts", [])
    cc_binary(
        name = name,
        **kwargs
    )

def rcllite_cc_test(name, **kwargs):
    """cc_test with the rcllite default copts/defines/linkopts applied.

    Args:
      name: target name, passed through.
      **kwargs: any other cc_test attributes; user copts/defines/linkopts
        are prepended to the rcllite defaults.
    """
    kwargs["copts"] = RCLLITE_DEFAULT_COPTS + kwargs.get("copts", [])
    kwargs["defines"] = RCLLITE_DEFAULT_DEFINES + kwargs.get("defines", [])
    kwargs["linkopts"] = RCLLITE_DEFAULT_LINKOPTS + kwargs.get("linkopts", [])
    cc_test(
        name = name,
        **kwargs
    )
