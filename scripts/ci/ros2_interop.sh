#!/usr/bin/env bash
# ROS 2 Humble interop tests for rcllite — the AGENTS.md interop matrix,
# executable in one shot.  Runs INSIDE a ros:humble container (the GitHub
# Actions CI job, or locally on any machine with Docker):
#
#   docker run --rm -v "$PWD":/work -w /work ros:humble \
#     bash scripts/ci/ros2_interop.sh /work
#
# Builds the example binaries with Bazel, then cross-checks them against the
# stock ros2 CLI / rclpy over CycloneDDS:
#   1. rcllite talker   -> `ros2 topic echo`          (topic pub, wire CDR)
#   2. `ros2 topic pub` -> rcllite listener           (topic sub, wire CDR)
#   3. `ros2 service call` -> rcllite service server  (service server)
#   4. rcllite service client -> rclpy server         (service client)
#   5. `ros2 node list` sees the rcllite nodes        (ros_discovery_info)
#   6. `ros2 param get/set` on an rcllite node        (parameters)
#
# Env overrides: BAZEL_OUTPUT_USER_ROOT, ROS_DOMAIN_ID.  Disk/repository
# caches are not wired here — in CI, bazel-contrib/setup-bazel provides them
# through the user bazelrc it writes; standalone runs just build cold.
set -euo pipefail

WORK="${1:-$PWD}"
cd "$WORK"

export ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-77}"
export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp
export CYCLONEDDS_URI="file://$WORK/docker/cyclonedds.xml"
BAZEL_ROOT="${BAZEL_OUTPUT_USER_ROOT:-/tmp/bazel-root}"

# ROS setup scripts reference unbound vars; set -u breaks them (and is only
# needed for this script's own logic, re-enabled right after).
set +u
source /opt/ros/humble/setup.bash
set -u

PASS=0
FAIL=0
PIDS=/tmp/rcllite-ci-pids
: >"$PIDS"
trap 'kill $(cat "$PIDS") 2>/dev/null || true' EXIT

say() { printf '\n========== %s\n' "$*"; }
start_node() {
  local n=$1
  shift
  "$@" >"/tmp/$n.log" 2>&1 &
  echo $! >>"$PIDS"
  echo $!
}
wait_log() { # <file> <pattern> [timeout_s]
  local f=$1 p=$2 t=${3:-60} i=0
  until grep -q "$p" "$f" 2>/dev/null; do
    if [ "$i" -ge "$t" ]; then
      echo "TIMEOUT (${t}s) waiting for '$p' in $f; last lines:"
      tail -n 8 "$f" 2>/dev/null || true
      return 1
    fi
    sleep 1
    i=$((i + 1))
  done
}

say "container bootstrap: rmw_cyclonedds + bazelisk"
apt-get update -qq
apt-get install -y -qq ros-humble-rmw-cyclonedds-cpp >/dev/null
# CI provides bazelisk (setup-bazel); standalone container runs fetch it here.
if ! command -v bazelisk >/dev/null 2>&1; then
  curl -fsSL -o /usr/local/bin/bazelisk \
    https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64
  chmod +x /usr/local/bin/bazelisk
fi

say "build examples (bazelisk)"
bazelisk --output_user_root="$BAZEL_ROOT" \
  build \
  //examples:rcllite_talker \
  //examples:rcllite_listener \
  //examples:rcllite_service_server \
  //examples:rcllite_service_client \
  //examples:rcllite_parameters_demo
BIN=bazel-bin/examples

# 1. topic pub (rcllite -> ros2): the talker streams Parameter messages on
#    /chatter at 1 Hz; echo with type autodetection must receive one.
say "1/6 rcllite talker -> ros2 topic echo"
start_node talker "$BIN/rcllite_talker" >/dev/null
# Capture first, assert second: `echo | grep -q` under pipefail can exit
# nonzero via SIGPIPE even when the message arrived, and capturing keeps the
# payload visible in the failure message.
echoed=$(timeout 90 ros2 topic echo --once /chatter 2>/dev/null || true)
if printf '%s\n' "$echoed" | grep -q "Hello, world!"; then
  PASS=$((PASS + 1))
else
  FAIL=$((FAIL + 1))
  echo "FAIL: nothing received on /chatter via ros2 topic echo; got:"
  printf '%s\n' "$echoed"
fi

# 2. topic sub (ros2 -> rcllite): publish from the CLI, the listener callback
#    prints the payload into its log.
say "2/6 ros2 topic pub -> rcllite listener"
start_node listener "$BIN/rcllite_listener" >/dev/null
sleep 3
if timeout 90 ros2 topic pub --times 5 /chatter rcl_interfaces/msg/Parameter \
  "{name: 'hi from ros2'}" >/dev/null 2>&1 &&
  wait_log /tmp/listener.log "hi from ros2" 60; then
  PASS=$((PASS + 1))
else
  FAIL=$((FAIL + 1))
  echo "FAIL: listener did not hear the ros2-published message"
fi

# 3. service server (ros2 -> rcllite): the server hashes each requested name
#    into an integer (type=2); ask for two names and require two distinct
#    in-range hashes, so a constant/garbage response of the right shape cannot
#    pass.  Humble's CLI prints responses as Python repr, not YAML.
say "3/6 ros2 service call -> rcllite service server"
SRV_PID=$(start_node srv "$BIN/rcllite_service_server")
if ! wait_log /tmp/srv.log "ready:" 30; then
  FAIL=$((FAIL + 1))
  echo "FAIL: rcllite service server did not become ready"
else
  out=$(timeout 90 ros2 service call /get_parameters rcl_interfaces/srv/GetParameters \
    "{names: [ci_probe, ci_other]}" 2>/dev/null || true)
  vals=$(printf '%s\n' "$out" | grep -oP 'integer_value=\K[0-9]+' || true)
  v1=$(printf '%s\n' "$vals" | sed -n 1p)
  v2=$(printf '%s\n' "$vals" | sed -n 2p)
  if printf '%s\n' "$out" | grep -q "type=2" &&
    [ -n "$v2" ] && [ "$v1" != "$v2" ] &&
    [ "$v1" -le 65535 ] && [ "$v2" -le 65535 ]; then
    PASS=$((PASS + 1))
  else
    FAIL=$((FAIL + 1))
    echo "FAIL: unexpected GetParameters response: $out"
  fi
fi
kill "$SRV_PID" 2>/dev/null || true
sleep 2

# 4. service client (rcllite -> ros2): a stock rclpy node hosts GetParameters
#    at /get_parameters; the rcllite client must correlate the responses.
say "4/6 rcllite service client -> rclpy service server"
cat >/tmp/ros_param_server.py <<'EOF'
import rclpy
from rcl_interfaces.msg import ParameterValue
from rcl_interfaces.srv import GetParameters
from rclpy.node import Node

rclpy.init()
node = Node('ros_get_parameters_server')


def cb(req, resp):
    for n in req.names:
        v = ParameterValue()
        v.type = 4  # PARAMETER_STRING
        v.string_value = 'value-of-' + n
        resp.values.append(v)
    return resp


node.create_service(GetParameters, 'get_parameters', cb)
rclpy.spin(node)
EOF
start_node rossrv python3 /tmp/ros_param_server.py >/dev/null
if timeout 60 "$BIN/rcllite_service_client" >/tmp/client.log 2>&1 &&
  grep -q "speed -> type=4 integer=0 string='value-of-speed'" /tmp/client.log; then
  PASS=$((PASS + 1))
else
  FAIL=$((FAIL + 1))
  echo "FAIL: rcllite client did not get the expected rclpy response:"
  cat /tmp/client.log 2>/dev/null || true
fi

# 5. graph discovery: ros_discovery_info published by rcllite nodes must make
#    them visible to `ros2 node list`.  Both checked nodes are rcllite and
#    still alive here (the rcllite service server was killed after test 3);
#    match lines exactly — an unanchored grep for '/get_parameters_server'
#    would silently match the rclpy '/ros_get_parameters_server' from test 4.
say "5/6 ros2 node list sees rcllite nodes"
ok=1
nodes=
for _ in $(seq 1 30); do
  nodes=$(ros2 node list 2>/dev/null || true)
  if printf '%s\n' "$nodes" | grep -qx '/talker' &&
    printf '%s\n' "$nodes" | grep -qx '/listener'; then
    ok=0
    break
  fi
  sleep 2
done
if [ "$ok" -eq 0 ]; then
  PASS=$((PASS + 1))
else
  FAIL=$((FAIL + 1))
  echo "FAIL: rcllite nodes missing from ros2 node list: $nodes"
fi

# 6. parameters: ros2 param set/get against the rcllite parameter services;
#    the demo mirrors every change in its 1 Hz status line.  Its lifetime is
#    the budget for the whole set -> observe -> get sequence, so keep it well
#    above the worst-case CLI/discovery latency on a loaded CI machine.
say "6/6 ros2 param get/set on rcllite node"
start_node demo "$BIN/rcllite_parameters_demo" 90 >/dev/null
sleep 3
if timeout 60 ros2 param set /parameters_demo my_string world >/dev/null 2>&1 &&
  wait_log /tmp/demo.log "my_string='world'" 20 &&
  timeout 60 ros2 param get /parameters_demo my_double 2>/dev/null | grep -q "3.14"; then
  PASS=$((PASS + 1))
else
  FAIL=$((FAIL + 1))
  echo "FAIL: ros2 param set/get against rcllite node"
fi

say "interop result: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
