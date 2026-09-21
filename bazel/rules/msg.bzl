"""Build-time generation of rcllite message/service headers.

rcllite_msg_library turns upstream ROS 2 interface definitions (.msg/.srv)
into C++ headers at build time (the same role rules_proto plays for .proto
files): a genrule invokes tools/msg_codegen.py and a cc_library exports the
generated headers so they are includable as

    #include "rcllite_types/<package>/<snake_case_name>.hpp"

Sources are normally file labels of an upstream interfaces repository
fetched with new_git_repository (see MODULE.bazel), so no generated headers
or interface copies are committed to this repository.
"""

load("@rules_cc//cc:cc_library.bzl", "cc_library")

def _camel_to_snake(s):
    # mirror of camel_to_snake() in tools/msg_codegen.py
    out = []
    for i in range(len(s)):
        ch = s[i]
        if ch.isupper() and i > 0 and (s[i - 1].islower() or s[i - 1].isdigit()):
            out.append("_")
        out.append(ch.lower())
    return "".join(out)

def rcllite_msg_library(name, package, files = [], deps = [], visibility = None):
    """Generate and export the C++ headers for one ROS 2 interface package.

    Args:
      name: target name (conventionally the package name).
      package: the ROS 2 package name the types belong to.
      files: labels of the .msg/.srv source files.
      deps: other rcllite_msg_library targets for nested type references.
      visibility: target visibility.
    """
    outs = []
    for f in files:
        base = f[f.rfind("/") + 1:].rsplit(".", 1)[0]
        outs.append("rcllite_types/%s/%s.hpp" % (package, _camel_to_snake(base)))

    # The generator writes <output>/rcllite_types/<package>/xxx.hpp; anchor
    # the output at this package's bin dir so it matches the declared outs.
    # (Must be invoked from a BUILD file in the top-level "msg" directory.)
    native.genrule(
        name = name + "_gen",
        srcs = files,
        outs = outs,
        tools = ["//tools:msg_codegen"],
        cmd = "$(location //tools:msg_codegen) --package " + package +
              " --output \"$(BINDIR)/msg\" --files $(SRCS)",
    )
    cc_library(
        name = name,
        hdrs = [":" + name + "_gen"],
        # expose headers as "rcllite_types/<package>/xxx.hpp"
        strip_include_prefix = "/msg",
        deps = ["//src/rcllite:cdr"] + deps,
        visibility = visibility,
    )
