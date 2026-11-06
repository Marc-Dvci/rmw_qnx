#!/bin/bash
# Boots the guest from Git Bash and waits until it answers on ssh. One boot in about twelve hangs under WHPX: after
# WAIT seconds without an answer it stops that guest (by its pidfile) and boots once more.
#   qnx/boot.sh [WAIT]
HERE="$(cd "$(dirname "$0")" && pwd)"
WAIT="${1:-90}"
PS1="$(cygpath -w "$HERE/run_whpx.ps1")"
for attempt in 1 2; do
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PS1" </dev/null
    start=$(date +%s)
    sleep 15
    while [ $(( $(date +%s) - start )) -lt "$WAIT" ]; do
        if bash "$HERE/g.sh" -o ConnectTimeout=5 true 2>/dev/null; then
            echo "guest up after $(( $(date +%s) - start )) s (attempt $attempt)"; exit 0
        fi
        sleep 2
    done
    echo "attempt $attempt: no answer after $WAIT s"
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PS1" -Stop </dev/null
    sleep 2
done
exit 1
