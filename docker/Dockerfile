# ROS 2 Humble interop environment for rcllite.
#
# rcllite speaks the ROS 2 wire protocol over CycloneDDS, so a stock ROS 2
# Humble installation configured with rmw_cyclonedds_cpp can:
#   * record rcllite publishers with `ros2 bag record`,
#   * replay bags into rcllite subscriptions with `ros2 bag play`,
#   * cross-check topics/services/parameters via the `ros2` CLI.
#
# Topic-level interop works with any rmw; services, parameters and the
# node graph additionally require rmw_cyclonedds_cpp (set below).
FROM ros:humble

ENV DEBIAN_FRONTEND=noninteractive

# Match rcllite's middleware: CycloneDDS.
ENV RMW_IMPLEMENTATION=rmw_cyclonedds_cpp

# See cyclonedds.xml: pick a usable network interface inside containers.
ENV CYCLONEDDS_URI=file:///interop/cyclonedds.xml

# Everything below ships with ros:humble already (re-listed so the image
# keeps working if the base variant changes) except the MCAP storage
# plugin, which adds an alternative bag format to the default sqlite3.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ros-humble-rmw-cyclonedds-cpp \
        ros-humble-demo-nodes-cpp \
        ros-humble-ros2bag \
        ros-humble-rosbag2-storage-mcap \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /interop
COPY cyclonedds.xml /interop/cyclonedds.xml

# Default working directory = the persistent host <-> container exchange
# space mounted by docker-compose.yml. One-off commands and shells
# (`docker compose run --rm play bash`) land where files are shared.
WORKDIR /works_dir

# The ros:humble entrypoint sources the ROS environment, so compose
# `command:` strings can invoke `ros2 ...` directly.
