# BUILD file used INSIDE the ros2/rcl_interfaces repository fetched via
# new_git_repository (see MODULE.bazel).  Exports the official interface
# definition files; the C++ headers are generated at build time by
# //bazel/rules/msg.bzl consumers in //msg.
package(default_visibility = ["//visibility:public"])

exports_files(
    glob([
        "builtin_interfaces/msg/*.msg",
        "rcl_interfaces/msg/*.msg",
        "rcl_interfaces/srv/*.srv",
        "rosgraph_msgs/msg/*.msg",
    ]),
)
