#!/bin/bash
# Records the kernel's own view of who ran on CPU 0 during pinv, with tracelogger on the instrumented kernel, for each
# transport under load. The .kev files come back to results/trace-<time>/; decode them on the host with
# scripts/trace_decode.sh (traceprinter from the SDP). Run from Git Bash on Windows:  bash scripts/pinv_trace.sh
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
STAMP="$(date +%Y%m%d-%H%M%S)"
D="$REPO/results/trace-$STAMP"
mkdir -p "$D"
bash "$REPO/qnx/boot.sh" || exit 1
for mode in msg udp shm; do
    bash "$REPO/qnx/g.sh" "cd /tmp && (tracelogger -w -s 2 -f /tmp/$mode.kev >/dev/null 2>&1 &) && sleep 0.3 && pinv $mode -n 100 -l 1 -v > /tmp/$mode.pinv; sleep 2.5; tail -1 /tmp/$mode.pinv; ls -la /tmp/$mode.kev"
    bash "$REPO/qnx/g.sh" "cat /tmp/$mode.kev" > "$D/$mode.kev"
    bash "$REPO/qnx/g.sh" "cat /tmp/$mode.pinv" > "$D/$mode.pinv"
done
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$REPO/qnx/run_whpx.ps1")" -Stop </dev/null
echo "saved $D"
