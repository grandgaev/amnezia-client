#!/bin/bash
set -euo pipefail
# UDP echo through the tunnel with routing disabled.
D=$(cd "$(dirname "$0")" && pwd)
STATE=$(mktemp /tmp/amnezia-router-probe.XXXXXX)
LOG=$(mktemp /tmp/amnezia-router-probe-log.XXXXXX)
trap '"$D/cleanup-state.sh" "$STATE"; rm -f "$LOG"' EXIT
E2E_STATE_FILE="$STATE" NOROUTING=1 SETUP_ONLY=1 timeout 60 "$D/run.sh" > "$LOG" 2>&1 || {
  tail -20 "$LOG"
  exit 1
}
# shellcheck disable=SC1090
. "$STATE"
sleep "${WAIT:-1}"
RESULT=""
for index in 1 2 3 4; do
  answer=$(ip netns exec "$E2E_NS_CLI" bash -c "echo -n hi$index | nc -u -w1 172.31.4.10 7")
  case "$answer" in from*) RESULT="$RESULT ok" ;; *) RESULT="$RESULT FAIL" ;; esac
done
echo "$RESULT"
