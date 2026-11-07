#!/bin/bash
# Builds rmw_qnx and the inversion scenario against the cross-built ROS 2 subset (qnx/ros2/build.sh), then packs the
# install tree for the guest as ros2-x86_64.tar.gz in the work folder.
#   qnx/ros2/build_ours.sh [WORK]
set -euo pipefail
WORK="${1:-$HOME/rmw_qnx-work}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
ROS="$WORK/build-files/ports/ros2/ros2-jazzy"
source "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
# Middlewares not built here are left out of colcon's graph (Connext needs RTI's SDK; the dynamic Fast DDS variant is not used).
RMWQNX_SKIP="${RMWQNX_SKIP-rmw_fastrtps_dynamic_cpp rmw_connextdds rmw_connextdds_common rti_connext_dds_cmake_module rmw_connextddsmicro}"
. "$WORK/venv/bin/activate"
mkdir -p "$ROS/src/rmwqnx"
ln -sfn "$REPO/rmw_qnx" "$ROS/src/rmwqnx/rmw_qnx"
ln -sfn "$REPO/bench/inversion_ros2" "$ROS/src/rmwqnx/inversion_ros2"
cd "$ROS"
export CPU=x86_64 CPUVARDIR=x86_64 CPUVAR=x86_64 ARCH=x86_64 QNX_SDP_VERSION=qnx800
export WORKSPACE="$ROS" PROJECT_ROOT="$ROS" INSTALL_BASE="$WORK/ros2-install/x86_64"
export MAKEFLAGS=-j4
colcon build --merge-install --cmake-clean-cache \
    --build-base "$WORK/ros2-build/x86_64" --install-base "$WORK/ros2-install/x86_64" \
    --packages-select rmw_qnx rmwqnx_inversion ${RMWQNX_SKIP:+--packages-ignore $RMWQNX_SKIP} "$@" \
    --cmake-args \
    -DCMAKE_TOOLCHAIN_FILE="$ROS/platform/qnx.nto.toolchain.cmake" -DCMAKE_MODULE_PATH="$ROS/modules" -DEXTRA_CMAKE_LINKER_FLAGS=-Wl,--no-as-needed \
    -DBUILD_TESTING:BOOL=OFF -DCMAKE_BUILD_TYPE=Release --no-warn-unused-cli -DCPU=x86_64
cd "$WORK/ros2-install"
# Run time needs the libraries, the executables and share/ament_index; the build-time files (headers, CMake files,
# environment hooks) are left out, which also keeps the file count within the guest's data partition.
tar -czf "$WORK/ros2-x86_64.tar.gz" --exclude='*/include' --exclude='*.a' --exclude='*/cmake' --exclude='*/hook' \
    --exclude='*/environment' --exclude='*.ps1' --exclude='*.dsv' --exclude='*.sh' --exclude='*.bash' --exclude='*.zsh' \
    --exclude='*.msg' --exclude='*.srv' --exclude='*.idl' --exclude='*.action' --exclude='*/src/*' x86_64
tar -tzf "$WORK/ros2-x86_64.tar.gz" | wc -l
ls -la "$WORK/ros2-x86_64.tar.gz"
