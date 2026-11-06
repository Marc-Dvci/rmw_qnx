#!/bin/bash
# Builds rmw_qnx and the inversion scenario against the cross-built ROS 2 subset (qnx/ros2/build.sh), then packs the
# install tree for the guest as ros2-x86_64.tar.gz in the work folder.
#   qnx/ros2/build_ours.sh [WORK]
set -euo pipefail
WORK="${1:-$HOME/rmw_qnx-work}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
ROS="$WORK/build-files/ports/ros2/ros2-jazzy"
source "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
. "$WORK/venv/bin/activate"
mkdir -p "$ROS/src/rmwqnx"
ln -sfn "$REPO/rmw_qnx" "$ROS/src/rmwqnx/rmw_qnx"
ln -sfn "$REPO/bench/inversion_ros2" "$ROS/src/rmwqnx/inversion_ros2"
cd "$ROS"
export CPU=x86_64 CPUVARDIR=x86_64 CPUVAR=x86_64 ARCH=x86_64
export WORKSPACE="$ROS" PROJECT_ROOT="$ROS" INSTALL_BASE="$WORK/ros2-install/x86_64"
export MAKEFLAGS=-j4
colcon build --merge-install \
    --build-base "$WORK/ros2-build/x86_64" --install-base "$WORK/ros2-install/x86_64" \
    --packages-select rmw_qnx rmwqnx_inversion "$@" \
    --cmake-args \
    -DCMAKE_TOOLCHAIN_FILE="$ROS/platform/qnx.nto.toolchain.cmake" -DCMAKE_MODULE_PATH="$ROS/modules" \
    -DBUILD_TESTING:BOOL=OFF -DCMAKE_BUILD_TYPE=Release --no-warn-unused-cli -DCPU=x86_64
cd "$WORK/ros2-install"
tar -czf "$WORK/ros2-x86_64.tar.gz" --exclude='*/include/*' --exclude='*.a' x86_64
ls -la "$WORK/ros2-x86_64.tar.gz"
