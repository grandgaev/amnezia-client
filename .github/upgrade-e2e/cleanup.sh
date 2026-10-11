#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# shellcheck source=.github/upgrade-e2e/guard.sh
source "$ROOT/.github/upgrade-e2e/guard.sh"
upgrade_e2e_guard
upgrade_e2e_require_owner
WORK_DIR=${UPGRADE_E2E_WORK_DIR:-$RUNNER_TEMP/amnezia-upgrade-e2e}
upgrade_e2e_require_runner_path "$WORK_DIR"

sudo docker rm -f amnezia-awg2 amnezia-awg2-pre-upgrade >/dev/null 2>&1 || true
sudo docker network rm amnezia-dns-net >/dev/null 2>&1 || true
sudo docker image rm -f amnezia-awg2 >/dev/null 2>&1 || true
sudo rm -rf /opt/amnezia
sudo rm -rf /opt/amnezia-e2e
rm -rf "$WORK_DIR"
sudo rm -f "$UPGRADE_E2E_OWNER_MARKER"
