#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# shellcheck source=.github/upgrade-e2e/guard.sh
source "$ROOT/.github/upgrade-e2e/guard.sh"
upgrade_e2e_guard
upgrade_e2e_require_owner
export UPGRADE_E2E_DISPOSABLE=1

BINARY=${1:?integration executable is required}
WORK_DIR=${UPGRADE_E2E_WORK_DIR:-$RUNNER_TEMP/amnezia-upgrade-e2e}
FIXTURE_DIR=$WORK_DIR/fixture
upgrade_e2e_require_runner_path "$WORK_DIR"
if [ -e "$WORK_DIR" ] || [ -L "$WORK_DIR" ]; then
  echo "Upgrade e2e work directory already exists" >&2
  exit 1
fi

mkdir -p "$WORK_DIR"
trap '"$ROOT/.github/upgrade-e2e/cleanup.sh"' EXIT

export UPGRADE_E2E_FIXTURE_DIR=$FIXTURE_DIR
export UPGRADE_E2E_USER=${UPGRADE_E2E_USER:-amnezia-e2e}
export UPGRADE_E2E_PASSWORD=${UPGRADE_E2E_PASSWORD:-amnezia-e2e-password}
export UPGRADE_E2E_PORT=${UPGRADE_E2E_PORT:-2222}

run_scenario() {
  local scenario=$1
  echo "== Upgrade scenario: $scenario"
  "$ROOT/.github/upgrade-e2e/seed-container.sh" "$FIXTURE_DIR"
  "$BINARY" "$scenario"
}

run_scenario refresh
run_scenario protocol

echo "== Upgrade scenario: rollback"
"$ROOT/.github/upgrade-e2e/seed-container.sh" "$FIXTURE_DIR"
sudo docker pull alpine:3.20 >/dev/null
"$ROOT/.github/upgrade-e2e/install-docker-conflict.sh"
"$BINARY" rollback
test ! -e /opt/amnezia-e2e/inject-docker-name-conflict
sudo rm -rf /opt/amnezia-e2e

echo "== Upgrade scenario: interrupted recovery"
"$ROOT/.github/upgrade-e2e/seed-container.sh" "$FIXTURE_DIR"
sudo docker update --restart=no amnezia-awg2 >/dev/null
sudo docker stop amnezia-awg2 >/dev/null
sudo docker rename amnezia-awg2 amnezia-awg2-pre-upgrade
"$BINARY" recovery

echo "All disposable upgrade scenarios passed"
