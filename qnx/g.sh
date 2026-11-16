#!/bin/bash
# Runs a command on the guest over ssh, from Git Bash on Windows (Git's ssh client; WSL cannot reach the forwarded
# port). The key is the one the image was built with.
#   qnx/g.sh 'command'
HERE="$(cd "$(dirname "$0")" && pwd)"
exec /usr/bin/ssh -i "$HERE/../.guest/ssh/k" -p 2232 -o BatchMode=yes -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10 -o ServerAliveInterval=30 \
    root@127.0.0.1 "$@"
