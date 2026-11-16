#!/bin/bash
# Guest checks of rmw_qnx (bench/inversion_ros2/src/rmw_checks.cpp), with ros2graphd running so the /dev/ros2 part
# is checked too. Assumes "COPY=1 bash scripts/inversion_ros2.sh up" has run after the build. Output to
# results/rmw_checks-<time>.txt.
#   bash scripts/rmw_checks.sh [check...]
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$REPO/results/rmw_checks-$(date +%Y%m%d-%H%M%S).txt"
{
    echo "# rmw_qnx guest checks, QEMU, WHPX, 2 vCPUs; $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    timeout 300 bash "$REPO/qnx/g.sh" "R=/data/ros2/x86_64; export LD_LIBRARY_PATH=\$R/lib AMENT_PREFIX_PATH=\$R ROS_LOG_DIR=/tmp/roslog RMW_IMPLEMENTATION=rmw_qnx; \$R/bin/ros2graphd >/tmp/graphd.log 2>&1 </dev/null & G=\$!; sleep 1; \$R/lib/rmwqnx_inversion/rmw_checks $*; echo exit=\$?; kill \$G" </dev/null
} 2>&1 | tee "$OUT"
echo "saved $OUT"
