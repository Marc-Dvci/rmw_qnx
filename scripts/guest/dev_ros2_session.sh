#!/bin/sh
# Runs on the guest: the /dev/ros2 browser with a talker and a service running, each command printed before its
# output, as a terminal would show it.
R=/data/ros2/x86_64
export LD_LIBRARY_PATH=$R/lib AMENT_PREFIX_PATH=$R ROS_LOG_DIR=/tmp/roslog ROS_DOMAIN_ID=0
export PATH=$R/bin:$R/lib/rmwqnx_inversion:$PATH RMW_IMPLEMENTATION=rmw_qnx
slay -f ros2graphd chatter inversion >/dev/null 2>&1
rm -rf /tmp/rmwqnx
run() { echo "# $*"; sh -c "$*"; echo; }
echo "# ros2graphd &"
ros2graphd </dev/null &
sleep 1
echo "# RMW_IMPLEMENTATION=rmw_qnx chatter talk 600 200 &        (a talker on /chatter, 5 messages a second)"
chatter talk 600 200 >/dev/null 2>&1 </dev/null &
echo "# RMW_IMPLEMENTATION=rmw_qnx chatter listen 1000 120 &      (a listener on /chatter)"
chatter listen 1000 120 >/dev/null 2>&1 </dev/null &
inversion init
echo "# RMW_IMPLEMENTATION=rmw_qnx inversion server &            (a service, /inversion)"
inversion server >/dev/null 2>&1 </dev/null &
sleep 4
echo
run "ls /dev/ros2"
run "ls /dev/ros2/topics"
run "ls -l /dev/ros2/topics"
run "cat /dev/ros2/topics/chatter"
run "sleep 2; grep -A1 latest /dev/ros2/topics/chatter"
run "cat /dev/ros2/topics/parameter_events"
run "ls /dev/ros2/services"
run "cat /dev/ros2/services/inversion"
slay -f chatter inversion ros2graphd >/dev/null 2>&1
