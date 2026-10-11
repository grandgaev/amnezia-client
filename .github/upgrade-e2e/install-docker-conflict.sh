#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# shellcheck source=.github/upgrade-e2e/guard.sh
source "$ROOT/.github/upgrade-e2e/guard.sh"
upgrade_e2e_guard
upgrade_e2e_require_owner

sudo install -d -m 0755 /opt/amnezia-e2e/bin
sudo install -m 0755 \
  "$ROOT/.github/upgrade-e2e/docker-conflict-wrapper.sh" \
  /opt/amnezia-e2e/bin/docker
printf 'run_id=%s\nrunner=github-hosted\nhost=%s\n' \
  "$UPGRADE_E2E_RUN_ID" "$UPGRADE_E2E_HOST" |
  sudo tee /opt/amnezia-e2e/context >/dev/null
sudo chmod 0600 /opt/amnezia-e2e/context
sudo touch /opt/amnezia-e2e/inject-docker-name-conflict
