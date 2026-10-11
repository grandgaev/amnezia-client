#!/bin/bash
set -euo pipefail

REAL_DOCKER=/usr/bin/docker
SENTINEL=/opt/amnezia-e2e/inject-docker-name-conflict
OWNER_MARKER=/run/amnezia-upgrade-e2e-owner
CONTEXT=/opt/amnezia-e2e/context

if [ "$EUID" -ne 0 ]; then
  echo "Upgrade e2e Docker wrapper must run as root" >&2
  exit 1
fi
owner=$(cat "$OWNER_MARKER" 2>/dev/null) || {
  echo "Upgrade e2e ownership marker is missing" >&2
  exit 1
}
grep -Fqx "run_id=$owner" "$CONTEXT" 2>/dev/null || {
  echo "Upgrade e2e wrapper ownership mismatch" >&2
  exit 1
}
grep -Fqx 'runner=github-hosted' "$CONTEXT" || exit 1
grep -Eq '^host=(127\.0\.0\.1|localhost|::1)$' "$CONTEXT" || exit 1

if [ "${1:-}" = run ] && [ -f "$SENTINEL" ]; then
  case " $* " in
    *" --name amnezia-awg2 "*)
      rm -f "$SENTINEL"
      "$REAL_DOCKER" create --name amnezia-awg2 alpine:3.20 sleep infinity >/dev/null
      echo "container name is already in use by container"
      ;;
  esac
fi
exec "$REAL_DOCKER" "$@"
