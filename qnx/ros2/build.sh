#!/bin/bash
# Cross-compiles the part of ROS 2 Jazzy that rmw_qnx and its tests need, for QNX 8.0 x86_64, with QNX's own patches
# and toolchain file from qnx-ports/build-files. Unlike build-files' build-ros2.sh it installs into the work folder
# only and leaves the SDP untouched. Run qnx/ros2/fetch.sh first.
#
#   qnx/ros2/build.sh [WORK] [PACKAGES...]    default WORK=~/rmw_qnx-work
#
# PACKAGES (colcon --packages-up-to) defaults to everything the inversion scenario and the comparison need. Packages
# that already finished are skipped, so a failed build resumes where it stopped. RMWQNX_SKIP names packages to leave
# out (for example "fastrtps rmw_fastrtps_cpp rmw_fastrtps_shared_cpp rmw_fastrtps_dynamic_cpp" to get to Cyclone
# DDS first).
#
# -Wl,--no-as-needed: build-files' toolchain file links shared objects with --as-needed ahead of the libraries qcc
# adds, so libc.so is dropped and libc pieces are copied from libcS.a into each library and exported. Their memchr
# then interposes on everyone else's, and every process that sets RMW_IMPLEMENTATION crashes in rcutils_strndup
# (docs/RESULTS.md, "A QNX build fault").
#
# Python packages (rclpy, rosidl_python) are left out: the guest's Python is 3.13, which build-files does not target,
# and nothing here needs Python on the guest.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${1:-$HOME/rmw_qnx-work}"
shift || true
PKGS="${*:-rclcpp rmw_cyclonedds_cpp rmw_fastrtps_cpp rmw_implementation example_interfaces std_msgs}"
ROS="$WORK/build-files/ports/ros2/ros2-jazzy"
source "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
. "$WORK/venv/bin/activate"
cd "$ROS"
if [ ! -f .rmwqnx-patched ]; then
    ./scripts/colcon-ignore.sh >/dev/null
    ./scripts/patch.sh
    touch .rmwqnx-patched
fi
# This repository's own patches on top of QNX's (each explains itself in its header).
if [ ! -f .rmwqnx-patched-2 ]; then
    tail -n +5 "$HERE/patches/rcutils-strndup.patch" | git -C src/ros2/rcutils apply -
    touch .rmwqnx-patched-2
fi
if [ ! -f .rmwqnx-patched-3 ]; then
    tail -n +3 "$HERE/patches/rttest-rusage.patch" | git -C src/ros2/realtime_support apply --directory=rttest -
    touch .rmwqnx-patched-3
fi
for p in rosidl_python rclpy ros2cli launch launch_ros rosbag2 rviz ros2cli_common_extensions sros2 system_tests; do
    [ -d "src/ros2/$p" ] && touch "src/ros2/$p/COLCON_IGNORE"
done
# numpy_vendor is listed by QNX's patches in some interface packages, for their Python bindings only.
touch modules/numpy_vendor/COLCON_IGNORE
# build-files leaves realtime_support (rttest, tlsf_cpp) out; pendulum_control needs it. Set aside, not removed.
[ -f src/ros2/realtime_support/COLCON_IGNORE ] && mv src/ros2/realtime_support/COLCON_IGNORE src/ros2/realtime_support/COLCON_IGNORE.set-aside
export CPU=x86_64 CPUVARDIR=x86_64 CPUVAR=x86_64 ARCH=x86_64 QNX_SDP_VERSION=qnx800
export WORKSPACE="$ROS" PROJECT_ROOT="$ROS" INSTALL_BASE="$WORK/ros2-install/x86_64"
export LC_NUMERIC="en_US.UTF-8" PYTHONPYCACHEPREFIX=/tmp
[ -f /usr/lib/x86_64-linux-gnu/libzstd.so.1 ] && export LD_PRELOAD="/usr/lib/x86_64-linux-gnu/libzstd.so.1"
# rmw_implementation's build-time default; at run time RMW_IMPLEMENTATION chooses.
export RMW_IMPLEMENTATION="${RMW_IMPLEMENTATION:-rmw_cyclonedds_cpp}"
export MAKEFLAGS=-j2
df -h /mnt/c | tail -1
# shellcheck disable=SC2086
# FRESH=1 reconfigures and rebuilds everything (after a change of flags).
# ONLY=1 rebuilds exactly the named packages.
SEL="--packages-up-to"
if [ -n "${FRESH:-}" ]; then RESUME="--cmake-clean-cache"; else RESUME="--packages-skip-build-finished"; fi
if [ -n "${ONLY:-}" ]; then RESUME=""; SEL="--packages-select"; fi
colcon build --merge-install --parallel-workers 2 $RESUME \
    --build-base "$WORK/ros2-build/x86_64" --install-base "$WORK/ros2-install/x86_64" \
    $SEL $PKGS ${RMWQNX_SKIP:+--packages-ignore $RMWQNX_SKIP} \
    --cmake-args \
    -DCMAKE_TOOLCHAIN_FILE="$ROS/platform/qnx.nto.toolchain.cmake" \
    -DCMAKE_MODULE_PATH="$ROS/modules" -DEXTRA_CMAKE_LINKER_FLAGS=-Wl,--no-as-needed \
    -DBUILD_TESTING:BOOL=${RMWQNX_TESTING:-OFF} -DQNX_TESTING=OFF -DCMAKE_BUILD_TYPE=Release -DTHIRDPARTY=FORCE \
    --no-warn-unused-cli -DCPU=x86_64 -DSECURITY=ON
df -h /mnt/c | tail -1
du -sh "$WORK/ros2-build" "$WORK/ros2-install"
