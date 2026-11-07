#!/bin/bash
# The whole ROS 2 kill test from a guest without the ROS 2 install: boot, remove /data/ros2, install, three repeats
# of each middleware with and without load, one traced loaded run per middleware, stop the guest.
# Run from Git Bash on Windows:  bash scripts/killtest_all.sh
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
S="$REPO/scripts/inversion_ros2.sh"
bash "$REPO/qnx/boot.sh" || exit 1
bash "$REPO/qnx/g.sh" 'rm -rf /data/ros2 /tmp/rmwqnx; echo "removed /data/ros2"' </dev/null
bash "$S" up || exit 1
REPEAT=3 bash "$S" run 200 rmw_fastrtps_cpp rmw_cyclonedds_cpp rmw_qnx
bash "$S" trace 100 rmw_fastrtps_cpp rmw_cyclonedds_cpp rmw_qnx
bash "$S" down
