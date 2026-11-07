#!/bin/sh
# Runs on the guest: one run of the ROS 2 inversion scenario.
#   run_inversion.sh RMW LOAD(0|1) REQUESTS [TRACE_FILE]
# With TRACE_FILE, tracelogger records the kernel's events for 3 s around the client's requests, and a PIDS line
# names the processes for scripts/trace_summary.py.
RMW=$1 LOAD=$2 N=$3 TRACE=$4
R=/data/ros2/x86_64
export LD_LIBRARY_PATH=$R/lib AMENT_PREFIX_PATH=$R ROS_LOG_DIR=/tmp/roslog ROS_DOMAIN_ID=7
export CYCLONEDDS_URI=file:///data/ros2/cyclonedds.xml RMW_IMPLEMENTATION=$RMW
I=$R/lib/rmwqnx_inversion/inversion
slay -f inversion >/dev/null 2>&1
rm -rf /tmp/rmwqnx
$I init || exit 1
$I server >/tmp/server.log 2>&1 </dev/null &
SP=$!
sleep 3
LP=-1
if [ "$LOAD" = 1 ]; then
    $I load >/dev/null 2>&1 </dev/null &
    LP=$!
fi
if [ -n "$TRACE" ]; then
    tracelogger -w -s 3 -f "$TRACE" >/dev/null 2>&1 </dev/null &
    sleep 0.3
fi
$I client "$N" >/tmp/client.log 2>&1 </dev/null &
CP=$!
wait $CP
grep RESULT /tmp/client.log | sed "s/\$/ load=$LOAD/"
grep -v RESULT /tmp/client.log | grep -v '^$' | tail -3
[ -n "$TRACE" ] && echo "PIDS client=$CP server=$SP load=$LP"
[ -n "$TRACE" ] && sleep 3
slay -f inversion >/dev/null 2>&1
sleep 1
grep -v '^$' /tmp/server.log | grep -v signal_handler | tail -3
