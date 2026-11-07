#!/bin/bash
# The priority-inversion scenario through ROS 2, on Fast DDS, Cyclone DDS and rmw_qnx, in the guest. Boots the guest,
# copies the ROS 2 install if the guest lacks it (or if COPY=1), runs each RMW with and without the medium-priority
# load, stops the guest. Output goes to stdout and results/inversion-ros2-<time>.txt.
# Run from Git Bash on Windows after qnx/ros2/build.sh and qnx/ros2/build_ours.sh:
#   bash scripts/inversion_ros2.sh [REQUESTS] [RMW...]
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
N="${1:-200}"
shift || true
RMWS="${*:-rmw_fastrtps_cpp rmw_cyclonedds_cpp rmw_qnx}"
G="bash $REPO/qnx/g.sh"
TAR="$(wsl.exe -d Ubuntu-22.04 -- bash -c 'echo $HOME' | tr -d '\r')/rmw_qnx-work/ros2-x86_64.tar.gz"
mkdir -p "$REPO/results"
OUT="$REPO/results/inversion-ros2-$(date +%Y%m%d-%H%M%S).txt"
bash "$REPO/qnx/boot.sh" || exit 1
if [ "${COPY:-0}" = 1 ] || ! $G 'test -f /data/ros2/x86_64/lib/librmw_qnx.so'; then
    wsl.exe -d Ubuntu-22.04 -- cat "$TAR" | $G 'mkdir -p /data/ros2 && cd /data/ros2 && rm -rf x86_64 && tar -xzf -'
fi
# Cyclone DDS on the loopback interface with unicast discovery: the guest's user-mode network has no multicast.
$G 'cat > /data/ros2/cyclonedds.xml' <<'EOF'
<CycloneDDS><Domain><General><Interfaces><NetworkInterface name="lo0"/></Interfaces>
<AllowMulticast>false</AllowMulticast></General><Discovery><ParticipantIndex>auto</ParticipantIndex>
<Peers><Peer address="127.0.0.1"/></Peers></Discovery></Domain></CycloneDDS>
EOF
ENV='export R=/data/ros2/x86_64; export LD_LIBRARY_PATH=$R/lib; export AMENT_PREFIX_PATH=$R; export ROS_LOG_DIR=/tmp/roslog; export ROS_DOMAIN_ID=7; export CYCLONEDDS_URI=file:///data/ros2/cyclonedds.xml; I=$R/lib/rmwqnx_inversion/inversion'
{
    echo "# inversion through ROS 2, QEMU, WHPX, 2 vCPUs, all three processes pinned to CPU 0; $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    for rmw in $RMWS; do
        for load in 0 1; do
            $G "$ENV; export RMW_IMPLEMENTATION=$rmw; rm -rf /tmp/rmwqnx; \$I init && (\$I server > /tmp/server.log 2>&1 &) ; sleep 3; if [ $load = 1 ]; then (\$I load &) ; fi; \$I client $N | grep RESULT | sed 's/\$/ load=$load/'; slay -f inversion >/dev/null 2>&1; sleep 1; tail -3 /tmp/server.log"
        done
    done
} 2>&1 | tee "$OUT"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$REPO/qnx/run_whpx.ps1")" -Stop </dev/null
echo "saved $OUT"
