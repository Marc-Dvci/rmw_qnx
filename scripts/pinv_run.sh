#!/bin/bash
# The transport-level priority-inversion test, end to end: boot the guest, run pinv over each transport with and
# without the medium-priority load, stop the guest. Output goes to stdout and to results/pinv-<time>.txt.
# Run from Git Bash on Windows:  bash scripts/pinv_run.sh [REQUESTS]
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
N="${1:-200}"
mkdir -p "$REPO/results"
OUT="$REPO/results/pinv-$(date +%Y%m%d-%H%M%S).txt"
bash "$REPO/qnx/boot.sh" || exit 1
{
    echo "# pinv, QEMU, WHPX, 2 vCPUs, all competitors pinned to CPU 0; $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    bash "$REPO/qnx/g.sh" 'uname -a; pidin info | head -4'
    for load in 0 1; do
        for mode in msg udp shm; do
            bash "$REPO/qnx/g.sh" "pinv $mode -n $N -l $load"
        done
    done
} 2>&1 | tee "$OUT"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$REPO/qnx/run_whpx.ps1")" -Stop </dev/null
echo "saved $OUT"
