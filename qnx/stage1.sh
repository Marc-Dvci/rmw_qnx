#!/bin/bash
# Builds the transport-level test (pinv) and a guest image holding it, in the WSL work folder.
#   qnx/stage1.sh [WORK]    default WORK=~/rmw_qnx-work
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${1:-$HOME/rmw_qnx-work}"
source "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
mkdir -p "$WORK/bin/x86_64"
make -s -C "$REPO/bench/pinv" OUT="$WORK/bin/x86_64"
RMWQNX_BIN="$WORK/bin/x86_64" RMWQNX_STARTUP="-f 0,0,200M" \
    bash "$REPO/qnx/image/build.sh" "$WORK/image" "$REPO/.guest/ssh/k.pub" > "$WORK/image-build.log" 2>&1 \
    || { tail -30 "$WORK/image-build.log"; exit 1; }
tail -3 "$WORK/image-build.log"
mkdir -p "$REPO/.guest/vm"
cp -f "$WORK/image/output/ifs.bin" "$WORK/image/output/disk-qemu.vmdk" "$WORK/image/output/disk-qemu" "$REPO/.guest/vm/"
ls -la "$REPO/.guest/vm"
