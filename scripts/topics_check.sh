#!/bin/bash
# Topics through rmw_qnx in the guest: a listener and a talker on /chatter, in two processes, then the same on Cyclone
# DDS for comparison. Assumes "scripts/inversion_ros2.sh up" has run. Output to results/topics-<time>.txt.
#   bash scripts/topics_check.sh [N]
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
N="${1:-20}"
OUT="$REPO/results/topics-$(date +%Y%m%d-%H%M%S).txt"
{
    echo "# chatter through ROS 2, QEMU, WHPX; $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    for rmw in rmw_qnx rmw_cyclonedds_cpp; do
        echo "## $rmw"
        timeout 90 bash "$REPO/qnx/g.sh" "R=/data/ros2/x86_64; export LD_LIBRARY_PATH=\$R/lib AMENT_PREFIX_PATH=\$R ROS_LOG_DIR=/tmp/roslog ROS_DOMAIN_ID=7 CYCLONEDDS_URI=file:///data/ros2/cyclonedds.xml RMW_IMPLEMENTATION=$rmw; C=\$R/lib/rmwqnx_inversion/chatter; \$C listen $N 20 > /tmp/listen.log 2>&1 </dev/null & L=\$!; sleep 3; \$C talk $N 100 2>&1 | grep CHATTER; wait \$L; head -3 /tmp/listen.log; grep CHATTER /tmp/listen.log" </dev/null
    done
} 2>&1 | tee "$OUT"
echo "saved $OUT"
