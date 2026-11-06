#!/bin/bash
# Cross-compiles the part of ROS 2 Jazzy that rmw_qnx and its tests need, for QNX 8.0 x86_64, with QNX's own patches
# and toolchain file from qnx-ports/build-files. Unlike build-files' build-ros2.sh it installs into the work folder
# only and leaves the SDP untouched. Run qnx/ros2/fetch.sh first.
#
#   qnx/ros2/build.sh [WORK] [extra colcon arguments]    default WORK=~/rmw_qnx-work
#
# Python packages (rclpy, rosidl_python) are left out: the guest's Python is 3.13, which build-files does not target,
# and nothing here needs Python on the guest.
set -euo pipefail
WORK="${1:-$HOME/rmw_qnx-work}"
shift || true
ROS="$WORK/build-files/ports/ros2/ros2-jazzy"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
source "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
. "$WORK/venv/bin/activate"
cd "$ROS"
if [ ! -f .rmwqnx-patched ]; then
    ./scripts/colcon-ignore.sh >/dev/null
    ./scripts/patch.sh
    touch .rmwqnx-patched
fi
for p in rosidl_python rclpy ros2cli launch launch_ros rosbag2 rviz ros2cli_common_extensions sros2 system_tests; do
    [ -d "src/ros2/$p" ] && touch "src/ros2/$p/COLCON_IGNORE"
done
# numpy_vendor is listed by QNX's patches in some interface packages, for their Python bindings only.
touch modules/numpy_vendor/COLCON_IGNORE
export CPU=x86_64 CPUVARDIR=x86_64 CPUVAR=x86_64 ARCH=x86_64 QNX_SDP_VERSION=qnx800
export WORKSPACE="$ROS" PROJECT_ROOT="$ROS" INSTALL_BASE="$WORK/ros2-install/x86_64"
export LC_NUMERIC="en_US.UTF-8" PYTHONPYCACHEPREFIX=/tmp
[ -f /usr/lib/x86_64-linux-gnu/libzstd.so.1 ] && export LD_PRELOAD="/usr/lib/x86_64-linux-gnu/libzstd.so.1"
export MAKEFLAGS=-j2
df -h /mnt/c | tail -1
colcon build --merge-install --parallel-workers 2 \
    --build-base "$WORK/ros2-build/x86_64" --install-base "$WORK/ros2-install/x86_64" \
    --packages-up-to rclcpp rmw_fastrtps_cpp rmw_cyclonedds_cpp rmw_implementation example_interfaces std_msgs \
    "$@" \
    --cmake-args \
    -DCMAKE_TOOLCHAIN_FILE="$ROS/platform/qnx.nto.toolchain.cmake" \
    -DCMAKE_MODULE_PATH="$ROS/modules" \
    -DBUILD_TESTING:BOOL=OFF -DQNX_TESTING=OFF -DCMAKE_BUILD_TYPE=Release -DTHIRDPARTY=FORCE \
    --no-warn-unused-cli -DCPU=x86_64 -DSECURITY=OFF
df -h /mnt/c | tail -1
du -sh "$WORK/ros2-build" "$WORK/ros2-install"
