#!/bin/bash
# Builds a QNX 8.0 image for QEMU x86_64 with mkqnximage, holding the rmw_qnx test binaries.
#
#   qnx/image/build.sh IMAGE_DIR SSH_PUBKEY [extra mkqnximage options]
#
# Environment:
#   RMWQNX_BIN       directory of target binaries copied to /system/bin (default qnx/build/x86_64)
#   RMWQNX_STARTUP   extra startup-x86 options. The Windows Hypervisor Platform needs "-f 0,0,200M": it does not
#                    report its APIC timer frequency in a form the startup library reads. KVM and real boards need
#                    nothing here.
#   RMWQNX_DATA_MB   extra space for the data partition (/data), default 3000
#
# procnto-smp-instr is the instrumented kernel, so tracelogger can record scheduling events. "-P 22" reserves
# priorities 22 and above for processes holding the priority ability; the tests use 10, 15 and 20.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
IMAGE_DIR="${1:?image directory}"
PUBKEY="${2:?ssh public key}"
shift 2
BIN="${RMWQNX_BIN:-$REPO/qnx/build/x86_64}"
source "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
mkdir -p "$IMAGE_DIR/local/snippets"
for f in "$HERE"/snippets/*.custom; do
    sed "s#__RMWQNX_BIN__#$BIN#g" "$f" >"$IMAGE_DIR/local/snippets/$(basename "$f")"
done
# No cluster advice (-c advice:...): the guest has 2 CPUs, and the tests pin to CPU 0, which is a predefined cluster.
printf '#define __STARTUP_OPTS__ %s\n' "${RMWQNX_STARTUP:-}" >"$IMAGE_DIR/local/snippets/definitions.custom"
cd "$IMAGE_DIR"
mkqnximage --noprompt --force --build --type=qemu --arch=x86_64 --graphics=no --hostname=rmwqnx --cpu=2 \
    --ram=4G --sys-size=64 --data-size="${RMWQNX_DATA_MB:-3000}" --proc="procnto-smp-instr -P 22" \
    --ssh-ident="$PUBKEY" "$@"
# The guest boots without snapshot=on, so QEMU stamps a CID into the descriptor at every boot. Without a CID line
# of its own, QEMU matches the "CID" inside "parentCID" and adds a line each time, until the 512-byte descriptor
# loses its extent line. A CID line first makes QEMU update it in place.
grep -q '^CID=' output/disk-qemu.vmdk || sed -i 's/^parentCID=/CID=fffffffe\nparentCID=/' output/disk-qemu.vmdk
ls -la output/ifs.bin output/disk-qemu.vmdk
