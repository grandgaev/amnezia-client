#!/bin/bash
set -euo pipefail
# usage: OBF_OVERRIDE="..." ./probe.sh
D=$(cd "$(dirname "$0")" && pwd)
STATE=$(mktemp /tmp/amnezia-router-probe.XXXXXX)
LOG=$(mktemp /tmp/amnezia-router-probe-log.XXXXXX)
trap '"$D/cleanup-state.sh" "$STATE"; rm -f "$LOG"' EXIT
E2E_STATE_FILE="$STATE" SETUP_ONLY=1 timeout 60 "$D/run.sh" > "$LOG" 2>&1 || {
  tail -20 "$LOG"
  exit 1
}
# shellcheck disable=SC1090
. "$STATE"
sleep 1
RESULT=""
for name in proxy.test other.test proxy.test other.test; do
  answer=$(ip netns exec "$E2E_NS_CLI" dig +short +time=2 +tries=1 @198.18.0.53 "$name" | tail -1)
  case "$answer" in 172.*) RESULT="$RESULT ok" ;; *) RESULT="$RESULT FAIL" ;; esac
done
echo "$RESULT"
