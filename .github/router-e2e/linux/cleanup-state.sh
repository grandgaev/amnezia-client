#!/bin/bash
set -u

if [ "$#" -ne 1 ] || [ ! -f "$1" ]; then
  echo "Usage: $0 <state-file>" >&2
  exit 2
fi

# The state file is produced by run.sh and contains shell-escaped values only.
# shellcheck disable=SC1090
. "$1"

for pid in $E2E_PIDS; do
  kill "$pid" 2>/dev/null || true
done
for pid in $E2E_PIDS; do
  wait "$pid" 2>/dev/null || true
done
ip netns del "$E2E_NS_CLI" 2>/dev/null || true
ip netns del "$E2E_NS_INET" 2>/dev/null || true
ip link del "$E2E_HCLI" 2>/dev/null || true
ip link del "$E2E_HINET" 2>/dev/null || true
rm -f "$E2E_SERVER_SOCKET" "$E2E_CLIENT_SOCKET" "$1"
