#!/bin/bash
# Builds and runs the host tests of rmw_qnx's core (Linux or WSL, any C++17 compiler, CMake 3.16+).
#   scripts/host_tests.sh [BUILD_DIR]     COVERAGE=1 also prints line and branch coverage of the core (gcovr)
#                                         SANITIZE=1 builds with AddressSanitizer and UndefinedBehaviorSanitizer
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
B="${1:-$HOME/rmw_qnx-work/host-build}"
cmake -S "$REPO/tests/core" -B "$B" -DCOVERAGE="${COVERAGE:+ON}" -DSANITIZE="${SANITIZE:+ON}" -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$B" -j4 >/dev/null
"$B/core_tests" --gtest_shuffle --gtest_brief=1
if [ -n "${COVERAGE:-}" ]; then
    # gcovr before 7 takes --branches; later versions --txt-metric branch.
    gcovr -r "$REPO" --filter "$REPO/rmw_qnx/core/" --branches -s "$B" 2>/dev/null | tail -4 ||
        gcovr -r "$REPO" --filter "$REPO/rmw_qnx/core/" --txt-metric branch -s "$B" | tail -4
    gcovr -r "$REPO" --filter "$REPO/rmw_qnx/core/" -s "$B" | tail -3
fi
