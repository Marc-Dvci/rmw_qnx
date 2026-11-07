#!/bin/bash
# ROS 2's test_rmw_implementation suite in the guest, on each middleware given (default rmw_qnx and Cyclone DDS).
# Build it first with:  RMWQNX_TESTING=ON ONLY=1 bash qnx/ros2/build.sh ~/rmw_qnx-work test_rmw_implementation
# Assumes "scripts/inversion_ros2.sh up" has run. Git Bash on Windows:  bash scripts/rmw_suite.sh [RMW...]
# Each test executable gets 120 s. Output to results/test_rmw_implementation-<time>.txt, then a summary line per
# middleware: tests run, passed, failed, and executables that crashed or timed out.
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
RMWS="${*:-rmw_qnx rmw_cyclonedds_cpp}"
G() { bash "$REPO/qnx/g.sh" "$@" </dev/null; }
RW="/mnt$(cygpath -u "$REPO")"
wsl.exe -d Ubuntu-22.04 -- bash -c "bash '$RW/qnx/ros2/pack_tests.sh' '$RW/.guest/test_rmw.tar.gz'"
bash "$REPO/qnx/g.sh" 'mkdir -p /data/ros2/test_rmw && cd /data/ros2/test_rmw && gzip -dc | tar -xf -' <"$REPO/.guest/test_rmw.tar.gz"
TESTS=$(G 'cd /data/ros2/test_rmw && ls test_*' | tr -d '\r')
OUT="$REPO/results/test_rmw_implementation-$(date +%Y%m%d-%H%M%S).txt"
{
    echo "# test_rmw_implementation (ROS 2 Jazzy), QEMU, WHPX; $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    for rmw in $RMWS; do
        for t in $TESTS; do
            echo "## $rmw $t"
            timeout 120 bash "$REPO/qnx/g.sh" "R=/data/ros2/x86_64; export LD_LIBRARY_PATH=\$R/lib AMENT_PREFIX_PATH=\$R ROS_LOG_DIR=/tmp/roslog ROS_DOMAIN_ID=11 CYCLONEDDS_URI=file:///data/ros2/cyclonedds.xml RMW_IMPLEMENTATION=$rmw; cd /tmp; rm -rf /tmp/rmwqnx; /data/ros2/test_rmw/$t 2>&1 | grep -E '^\[  (PASSED|FAILED)  \]|^\[==========\]|Segmentation|Memory fault|Abort'; echo \"exit=\$?\"" </dev/null
            [ $? -eq 124 ] && echo "TIMEOUT"
        done
    done
} 2>&1 | tr -d '\r' | tee "$OUT"
python "$REPO/scripts/suite_summary.py" "$OUT" | tee -a "$OUT"
echo "saved $OUT"
