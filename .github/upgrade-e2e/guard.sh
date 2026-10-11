#!/bin/bash

upgrade_e2e_guard() {
  if [ "${GITHUB_ACTIONS:-}" != true ] || [ "${RUNNER_ENVIRONMENT:-}" != github-hosted ]; then
    echo "Upgrade e2e is restricted to disposable GitHub-hosted runners" >&2
    return 1
  fi

  case "${UPGRADE_E2E_HOST:-127.0.0.1}" in
    127.0.0.1|localhost|::1) ;;
    *)
      echo "Upgrade e2e SSH target must be localhost" >&2
      return 1
      ;;
  esac

  if [ -z "${RUNNER_TEMP:-}" ] || [ -z "${GITHUB_RUN_ID:-}" ]; then
    echo "GitHub runner identity is unavailable" >&2
    return 1
  fi

  sudo -n true
  export UPGRADE_E2E_HOST=${UPGRADE_E2E_HOST:-127.0.0.1}
  export UPGRADE_E2E_RUN_ID="${GITHUB_RUN_ID}.${GITHUB_RUN_ATTEMPT:-1}"
  export UPGRADE_E2E_OWNER_MARKER=/run/amnezia-upgrade-e2e-owner
}

upgrade_e2e_require_runner_path() {
  local candidate=${1:?path is required}
  local runner_root candidate_path
  runner_root=$(realpath -m "$RUNNER_TEMP")
  candidate_path=$(realpath -m "$candidate")
  case "$candidate_path" in
    "$runner_root"/*) ;;
    *)
      echo "Upgrade e2e path must be inside RUNNER_TEMP: $candidate_path" >&2
      return 1
      ;;
  esac
}

upgrade_e2e_require_owner() {
  local owner
  owner=$(sudo cat "$UPGRADE_E2E_OWNER_MARKER" 2>/dev/null) || {
    echo "Upgrade e2e ownership marker is missing" >&2
    return 1
  }
  if [ "$owner" != "$UPGRADE_E2E_RUN_ID" ]; then
    echo "Upgrade e2e ownership marker belongs to another run" >&2
    return 1
  fi
}
