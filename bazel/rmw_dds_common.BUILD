# BUILD file used INSIDE the ros2/rmw_dds_common repository fetched via
# new_git_repository (see MODULE.bazel).  Exports the official interface
# definition files; the C++ headers are generated at build time by
# //bazel/rules/msg.bzl consumers in //msg.
package(default_visibility = ["//visibility:public"])

exports_files(
    glob(["rmw_dds_common/msg/*.msg"]),
)
