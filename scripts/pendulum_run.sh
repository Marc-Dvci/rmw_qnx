#!/bin/bash
# ROS 2's pendulum_control demo, unchanged, in the guest: pendulum_demo with the middleware chosen only by
# RMW_IMPLEMENTATION. Assumes "scripts/inversion_ros2.sh up" has run. Output to results/pendulum-<time>.txt.
#   bash scripts/pendulum_run.sh [ITERATIONS] [RMW...]     defaults: 2000, rmw_qnx rmw_cyclonedds_cpp
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
N="${1:-2000}"
shift || true
RMWS="${*:-rmw_qnx rmw_cyclonedds_cpp}"
OUT="$REPO/results/pendulum-$(date +%Y%m%d-%H%M%S).txt"
{
    echo "# pendulum_demo -i $N, QEMU, WHPX; $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    for rmw in $RMWS; do
        echo "## RMW_IMPLEMENTATION=$rmw pendulum_demo -i $N"
        timeout 150 bash "$REPO/qnx/g.sh" "R=/data/ros2/x86_64; export LD_LIBRARY_PATH=\$R/lib AMENT_PREFIX_PATH=\$R ROS_LOG_DIR=/tmp/roslog ROS_DOMAIN_ID=9 CYCLONEDDS_URI=file:///data/ros2/cyclonedds.xml RMW_IMPLEMENTATION=$rmw; cd /tmp; \$R/lib/pendulum_control/pendulum_demo -i $N 2>&1 | tail -25; echo exit=\$?" </dev/null
    done
} 2>&1 | tr -d '\r' | tee "$OUT"
echo "saved $OUT"
