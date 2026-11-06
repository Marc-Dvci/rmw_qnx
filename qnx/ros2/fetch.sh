#!/bin/bash
# Fetches ROS 2 Jazzy sources for QNX with qnx-ports/build-files (https://github.com/qnx-ports/build-files), which
# holds QNX's own patches and build script for ROS 2. Nothing from it is copied into this repository.
#   qnx/ros2/fetch.sh [WORK]    default WORK=~/rmw_qnx-work
set -euo pipefail
WORK="${1:-$HOME/rmw_qnx-work}"
mkdir -p "$WORK"
cd "$WORK"
[ -d venv ] || python3 -m venv venv
. venv/bin/activate
pip install -q -U pip vcstool colcon-common-extensions empy==3.3.4 lark catkin_pkg numpy
[ -d build-files ] || git clone -q --depth 1 https://github.com/qnx-ports/build-files.git
[ -d googletest ] || git clone -q --depth 1 -b qnx_v1.13.0 https://github.com/qnx-ports/googletest.git
cd build-files/ports/ros2/ros2-jazzy
mkdir -p src
vcs import --shallow --skip-existing src < ros2.repos
echo "fetched: $(ls src | wc -l) groups, $(du -sh src | cut -f1)"
