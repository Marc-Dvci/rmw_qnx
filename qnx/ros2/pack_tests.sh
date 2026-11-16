#!/bin/bash
# Packs the test executables of test_rmw_implementation for the guest (WSL).
#   qnx/ros2/pack_tests.sh OUT.tar.gz [WORK]
set -euo pipefail
OUT="${1:?output file}"
WORK="${2:-$HOME/rmw_qnx-work}"
cd "$WORK/ros2-build/x86_64/test_rmw_implementation"
mapfile -t T < <(find . -maxdepth 1 -type f -perm -u+x -name 'test_*' | sort)
tar -czf "$OUT" "${T[@]}"
echo "packed ${#T[@]} test executables"
