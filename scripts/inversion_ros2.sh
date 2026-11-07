#!/bin/bash
# The priority-inversion scenario through ROS 2, in the guest, on each RMW given (default: Fast DDS, Cyclone DDS,
# rmw_qnx), with and without the medium-priority load. Run from Git Bash on Windows after qnx/ros2/build.sh and
# qnx/ros2/build_ours.sh, in three steps so a long job never holds the machine's lock idle:
#   bash scripts/inversion_ros2.sh up                 # boot the guest, copy the ROS 2 install if it lacks it
#   bash scripts/inversion_ros2.sh run [N] [RMW...]   # N requests per run (default 200); appends to results/
#   bash scripts/inversion_ros2.sh down               # stop the guest
# COPY=1 with "up" copies the install even if the guest has one.
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
# Git's ssh on Windows does not exit after the remote command while its stdin stays open: every call that does
# not feed the guest gets /dev/null.
G() { bash "$REPO/qnx/g.sh" -o ServerAliveCountMax=4 "$@" </dev/null; }
GIN() { bash "$REPO/qnx/g.sh" -o ServerAliveCountMax=4 "$@"; }
STEP="${1:-run}"
shift || true
case "$STEP" in
up)
    bash "$REPO/qnx/boot.sh" || exit 1
    if [ "${COPY:-0}" = 1 ] || ! G 'test -f /data/ros2/x86_64/lib/librmw_qnx.so'; then
        # Through a file on the Windows side: a pipe out of wsl.exe is not binary-safe.
        RW="/mnt$(cygpath -u "$REPO")"
        wsl.exe -d Ubuntu-22.04 -- bash -c "cp \$HOME/rmw_qnx-work/ros2-x86_64.tar.gz '$RW/.guest/ros2-x86_64.tar.gz'"
        GIN 'mkdir -p /data/ros2 && cat > /data/ros2/ros2-x86_64.tar.gz' <"$REPO/.guest/ros2-x86_64.tar.gz"
        G 'cd /data/ros2 && rm -rf x86_64 && gzip -dc ros2-x86_64.tar.gz | tar -xf - && ls x86_64/lib | wc -l'
    fi
    # Cyclone DDS on the loopback interface with unicast discovery: the guest's user-mode network has no multicast.
    GIN 'cat > /data/ros2/cyclonedds.xml' <<'EOF'
<CycloneDDS><Domain><General><Interfaces><NetworkInterface name="lo0"/></Interfaces>
<AllowMulticast>false</AllowMulticast></General><Discovery><ParticipantIndex>auto</ParticipantIndex>
<Peers><Peer address="127.0.0.1"/></Peers></Discovery></Domain></CycloneDDS>
EOF
    ;;
run)
    N="${1:-200}"
    shift || true
    RMWS="${*:-rmw_fastrtps_cpp rmw_cyclonedds_cpp rmw_qnx}"
    ENV='export R=/data/ros2/x86_64; export LD_LIBRARY_PATH=$R/lib; export AMENT_PREFIX_PATH=$R; export ROS_LOG_DIR=/tmp/roslog; export ROS_DOMAIN_ID=7; export CYCLONEDDS_URI=file:///data/ros2/cyclonedds.xml; I=$R/lib/rmwqnx_inversion/inversion'
    mkdir -p "$REPO/results"
    OUT="$REPO/results/inversion-ros2-$(date +%Y%m%d-%H%M%S).txt"
    {
        echo "# inversion through ROS 2, QEMU, WHPX, 2 vCPUs, all three processes pinned to CPU 0; $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        for rmw in $RMWS; do
            for load in 0 1; do
                G "$ENV; export RMW_IMPLEMENTATION=$rmw; rm -rf /tmp/rmwqnx; \$I init && (\$I server > /tmp/server.log 2>&1 </dev/null &) ; sleep 3; if [ $load = 1 ]; then (\$I load >/dev/null 2>&1 &) ; fi; \$I client $N | grep RESULT | sed 's/\$/ load=$load/'; slay -f inversion >/dev/null 2>&1; sleep 1; grep -v '^\$' /tmp/server.log | tail -3"
            done
        done
    } 2>&1 | tee "$OUT"
    echo "saved $OUT"
    ;;
down)
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$REPO/qnx/run_whpx.ps1")" -Stop </dev/null
    ;;
*)
    echo "usage: $0 up|run|down" >&2
    exit 64
    ;;
esac
