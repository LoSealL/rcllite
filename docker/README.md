# docker — ROS 2 Humble interop environment

This directory packages a stock **ROS 2 Humble** environment that speaks
the same wire protocol as **rcllite** (CycloneDDS). It is used for three
jobs:

1. **Record** live traffic — including topics published by rcllite nodes —
   into standard rosbag2 bags with `ros2 bag record`.
2. **Replay** existing rosbag2 bags with `ros2 bag play`; the bag's topics
   appear as ordinary ROS 2 publishers that rcllite nodes subscribe to.
3. **Cross-check** a running rcllite system with the `ros2` CLI
   (`topic` / `service` / `param` / `node`), mirroring the interop matrix
   documented in the repository's `AGENTS.md`.

## Contents

| File | Purpose |
|---|---|
| `Dockerfile` | `ros:humble` base + `rmw_cyclonedds_cpp` + rosbag2 (sqlite3 + MCAP) + demo nodes |
| `docker-compose.yml` | `talker` / `listener` demo, `record`, `play` services, selected via compose profiles |
| `cyclonedds.xml` | Makes CycloneDDS pick a usable network interface inside containers |
| `bags/` | Recorded bags land here (created on demand) |
| `works_dir/` | Persistent host ↔ container exchange space, mounted at `/works_dir` in every service (gitignored; only the mount point is tracked) |

## Quick start

```bash
cd docker
docker compose build

# smoke test: standard ROS 2 talker <-> listener over CycloneDDS
docker compose --profile demo up
# stop with Ctrl+C
```

## Recording a bag

```bash
# record the demo talker (run both profiles at once)
docker compose --profile demo --profile record up
# Ctrl+C stops everything; record finalizes bags/<BAG_NAME>/metadata.yaml
```

Bags are written to `./bags/<BAG_NAME>` (`BAG_NAME` defaults to `demo`).
Useful variants:

```bash
# record everything visible on the domain, under a custom name
BAG_NAME=run1 docker compose --profile record up

# one-shot recording of specific topics only
docker compose run --rm record ros2 bag record /chatter -o /bags/chatter

# MCAP storage instead of sqlite3
docker compose run --rm record ros2 bag record --all --storage mcap -o /bags/demo-mcap
```

Stopping the `record` service cleanly matters: the service sets
`stop_signal: SIGINT`, so rosbag2 finalizes `metadata.yaml` on `Ctrl+C` or
`docker compose stop record`. A hard kill (or `docker kill`) leaves a bag
without metadata that `ros2 bag play` refuses to open.

## Replaying a bag

```bash
docker compose --profile play up                    # replays bags/demo in a loop
BAG_PATH=demo-mcap docker compose --profile play up # pick another bag

# one-off commands (no profile flag needed for `run`)
docker compose run --rm play ros2 bag info /bags/demo
docker compose run --rm play ros2 bag play /bags/demo --rate 2.0
docker compose run --rm play bash                   # sourced shell, starts in /works_dir
```

`ros2 bag play` publishes the bag's topics like any ROS 2 node, so any
subscriber on the DDS domain sees them — rcllite subscriptions included.

## Talking to rcllite from these containers

DDS discovery between the Docker Desktop VM and a Windows/macOS host does
not work; host ↔ container interop requires **native Linux Docker**. There:

1. Uncomment `network_mode: host` in the `x-ros2` anchor of
   `docker-compose.yml` — one line, applies to every service.
2. Match the DDS domain: `ROS_DOMAIN_ID` (defaults to `0` here) must equal
   the rcllite process's domain.
3. Start the rcllite node on the host, then:

```bash
# visibility check first
docker compose run --rm play ros2 topic list

# record rcllite publishers into a bag
docker compose --profile record up

# replay a bag into rcllite subscriptions
docker compose --profile play up
```

As per the interop matrix: topics record and replay with any rmw; services
and parameters additionally need `rmw_cyclonedds_cpp`, which the image sets
via `RMW_IMPLEMENTATION`.

## Notes

- `works_dir/` is the persistent exchange path between the host and every
  container (`/works_dir` inside): drop topology JSONs, scripts or export
  files there and they show up in any service, including one-off shells —
  which start in that directory. Its contents are gitignored; `bags/`
  remains the dedicated output of `record` and input of `play`.
- `ROS_DOMAIN_ID` is configurable per invocation:
  `ROS_DOMAIN_ID=7 docker compose --profile record up`.
- Recording passes `--storage sqlite3` explicitly (the Humble default);
  the MCAP plugin is installed for `--storage mcap`.
- `bags/` is host-side state shared by the `record` and `play` services
  through the same bind mount.
