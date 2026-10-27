#!/bin/bash
# Key resolved from the script location (not the CWD): it works
# whether it is run from examples/example_67/ or from any other directory
# (e.g. examples/example_67/run_ssh_client.sh from the repo root).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KEY="$SCRIPT_DIR/../caos_ed25519/id_ed25519"

# Optional IP as first argument (default 192.168.10.67)
IP="${1:-192.168.10.67}"

ssh -o IdentitiesOnly=yes -v -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no \
    -i "$KEY" "$IP" -p 22
