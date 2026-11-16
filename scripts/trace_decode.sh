#!/bin/bash
# Decodes the .kev files of one pinv_trace.sh run with the SDP's traceprinter, in WSL.
#   scripts/trace_decode.sh results/trace-<time>
set -euo pipefail
D="$(cd "${1:?trace directory}" && pwd)"
source "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
for k in "$D"/*.kev; do
    traceprinter -f "$k" > "${k%.kev}.trace"
    echo "$(basename "$k"): $(wc -l < "${k%.kev}.trace") lines"
done
