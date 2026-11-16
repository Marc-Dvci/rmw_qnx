#!/bin/bash
# Records a real terminal session of the /dev/ros2 browser in the guest to docs/dev_ros2_session.txt.
# Assumes "scripts/inversion_ros2.sh up" has run. Git Bash on Windows:  bash scripts/dev_ros2_demo.sh
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
bash "$REPO/qnx/g.sh" 'cat > /data/ros2/dev_ros2_session.sh' <"$REPO/scripts/guest/dev_ros2_session.sh"
{
    echo "# QNX SDP 8.0, x86_64 guest (QEMU, WHPX), recorded $(date -u +%Y-%m-%d) with scripts/dev_ros2_demo.sh"
    timeout 120 bash "$REPO/qnx/g.sh" 'sh /data/ros2/dev_ros2_session.sh 2>&1' </dev/null
} | tr -d '\r' | tee "$REPO/docs/dev_ros2_session.txt"
