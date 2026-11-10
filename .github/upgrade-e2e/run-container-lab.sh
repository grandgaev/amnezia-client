#!/bin/bash
set -euo pipefail
umask 077

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BINARY=${1:?macOS integration executable is required}
LAB_SERVER=${UPGRADE_E2E_LAB_SERVER:?UPGRADE_E2E_LAB_SERVER must name the authorized SSH server}
ARTIFACT_DIR=${UPGRADE_E2E_ARTIFACT_DIR:-$ROOT/artifacts/upgrade-container-lab}
TIMEOUT_SECONDS=${UPGRADE_E2E_LAB_TIMEOUT_SECONDS:-5400}

test -x "$BINARY"
command -v ssh >/dev/null
command -v openssl >/dev/null
command -v python3 >/dev/null
command -v nc >/dev/null

nonce=$(openssl rand -hex 24)
short_nonce=${nonce:0:12}
container_name=amnezia-upgrade-e2e-lab-$short_nonce
password=$(openssl rand -hex 24)
image_id=
container_id=
tunnel_pid=
watchdog_pid=
WORK_DIR=
redactions_file=
mkdir -p "$ARTIFACT_DIR"
chmod 0700 "$ARTIFACT_DIR"

case "$TIMEOUT_SECONDS" in
  ''|*[!0-9]*) exit 2 ;;
esac
test "$TIMEOUT_SECONDS" -ge 300

remote_docker() {
  local command
  printf -v command '%q ' docker "$@"
  ssh -o BatchMode=yes -o ConnectTimeout=10 "$LAB_SERVER" "${command% }"
}

redact_artifacts() {
  if [ -z "$redactions_file" ] || [ ! -f "$redactions_file" ]; then
    return
  fi
  python3 - "$ARTIFACT_DIR" "$redactions_file" <<'PY'
from pathlib import Path
import re
import sys

artifact_dir = Path(sys.argv[1])
secrets = [line for line in Path(sys.argv[2]).read_bytes().splitlines() if line]
for path in artifact_dir.iterdir():
    if not path.is_file():
        continue
    data = path.read_bytes()
    redacted = data
    for secret in secrets:
        redacted = redacted.replace(secret, b"[REDACTED]")
    redacted = re.sub(rb'(?<![A-Za-z0-9+/])[A-Za-z0-9+/]{43}=(?![A-Za-z0-9+/=])',
                      b'[REDACTED-KEY]', redacted)
    redacted = re.sub(rb'(HeaderProtectionKey\s*=\s*)[^\s\\]+',
                      rb'\1[REDACTED-KEY]', redacted)
    if redacted != data:
        path.write_bytes(redacted)
PY
}

watchdog() {
  local sleep_pid=
  trap 'if [ -n "$sleep_pid" ]; then kill "$sleep_pid" >/dev/null 2>&1 || true; wait "$sleep_pid" >/dev/null 2>&1 || true; fi; exit 0' TERM INT
  sleep "$TIMEOUT_SECONDS" &
  sleep_pid=$!
  wait "$sleep_pid"
  kill -TERM "$$"
}

cleanup() {
  local status=$?
  local candidate_ids
  trap - EXIT INT TERM
  if [ -n "$tunnel_pid" ]; then
    kill "$tunnel_pid" >/dev/null 2>&1 || true
    wait "$tunnel_pid" >/dev/null 2>&1 || true
  fi
  if [ -n "$watchdog_pid" ]; then
    kill "$watchdog_pid" >/dev/null 2>&1 || true
    wait "$watchdog_pid" >/dev/null 2>&1 || true
  fi
  redact_artifacts
  if [ -n "$WORK_DIR" ]; then
    rm -rf "$WORK_DIR"
  fi
  if [ -z "$container_id" ]; then
    candidate_ids=$(remote_docker ps -aq \
      --filter "name=^/${container_name}$" \
      --filter "label=com.amnezia.upgrade-e2e.outer-run=$nonce" 2>/dev/null || true)
    if [ "$(printf '%s\n' "$candidate_ids" | sed '/^$/d' | wc -l | tr -d ' ')" = 1 ]; then
      container_id=$candidate_ids
    fi
  fi
  if [ -n "$container_id" ]; then
    ssh -o BatchMode=yes "$LAB_SERVER" sh -s -- "$container_id" "$nonce" <<'REMOTE' || true
set -eu
id=$1
nonce=$2
test "$(docker inspect -f '{{ index .Config.Labels "com.amnezia.upgrade-e2e.outer-run" }}' "$id")" = "$nonce"
docker logs "$id" 2>&1 || true
docker rm -f "$id" >/dev/null
REMOTE
  fi
  if [ -z "$image_id" ]; then
    candidate_ids=$(remote_docker image ls -q \
      --filter "label=com.amnezia.upgrade-e2e.outer-run=$nonce" 2>/dev/null || true)
    if [ "$(printf '%s\n' "$candidate_ids" | sed '/^$/d' | sort -u | wc -l | tr -d ' ')" = 1 ]; then
      image_id=$(printf '%s\n' "$candidate_ids" | sed '/^$/d' | sort -u)
    fi
  fi
  if [ -n "$image_id" ]; then
    ssh -o BatchMode=yes "$LAB_SERVER" sh -s -- "$image_id" "$nonce" <<'REMOTE' || true
set -eu
id=$1
nonce=$2
test "$(docker image inspect -f '{{ index .Config.Labels "com.amnezia.upgrade-e2e.outer-run" }}' "$id")" = "$nonce"
docker image rm -f "$id" >/dev/null
REMOTE
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 124' INT TERM
watchdog &
watchdog_pid=$!

image_id=$(
  COPYFILE_DISABLE=1 tar --format=ustar -C "$ROOT" -cf - \
    .github/upgrade-e2e/container-lab \
    client/server_scripts/awg |
    ssh -o BatchMode=yes "$LAB_SERVER" \
      env DOCKER_BUILDKIT=0 docker build --quiet \
        --cpu-period 100000 \
        --cpu-quota 50000 \
        --memory 512m \
        --memory-swap 1536m \
        --label "com.amnezia.upgrade-e2e.outer-run=$nonce" \
        -f .github/upgrade-e2e/container-lab/Dockerfile -
)

container_id=$(remote_docker create \
  --name "$container_name" \
  --label "com.amnezia.upgrade-e2e.outer-run=$nonce" \
  --privileged \
  --cgroupns=private \
  --cpus 0.5 \
  --memory 512m \
  --memory-swap 1536m \
  -e "UPGRADE_E2E_LAB_NONCE=$nonce" \
  -e "UPGRADE_E2E_PASSWORD=$password" \
  -e DOCKER_TLS_CERTDIR= \
  -p 127.0.0.1::22 \
  "$image_id")

test "$(remote_docker inspect -f '{{.State.Status}}' "$container_id")" = created
test "$(remote_docker inspect -f '{{.HostConfig.CgroupnsMode}}' "$container_id")" = private
test "$(remote_docker inspect -f '{{.HostConfig.NanoCpus}}' "$container_id")" = 500000000
test "$(remote_docker inspect -f '{{.HostConfig.Memory}}' "$container_id")" = 536870912
test "$(remote_docker inspect -f '{{.HostConfig.MemorySwap}}' "$container_id")" = 1610612736
remote_docker start "$container_id" >/dev/null

for _ in $(seq 1 480); do
  health=$(remote_docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{end}}' "$container_id")
  if [ "$health" = healthy ]; then
    break
  fi
  if [ "$health" = unhealthy ]; then
    remote_docker logs "$container_id" >&2
    exit 1
  fi
  sleep 1
done
test "${health:-}" = healthy

remote_port=$(remote_docker port "$container_id" 22/tcp | sed -n 's/^127\.0\.0\.1://p')
test -n "$remote_port"
local_port=$(python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
)
ssh -o BatchMode=yes -o ExitOnForwardFailure=yes -N \
  -L "127.0.0.1:$local_port:127.0.0.1:$remote_port" \
  "$LAB_SERVER" &
tunnel_pid=$!
for _ in $(seq 1 50); do
  nc -z 127.0.0.1 "$local_port" >/dev/null 2>&1 && break
  kill -0 "$tunnel_pid"
  sleep 0.1
done
nc -z 127.0.0.1 "$local_port"

WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/amnezia-upgrade-container-lab.XXXXXX")
fixture=$WORK_DIR/fixture
mkdir -p "$fixture"
chmod 0700 "$fixture"
redactions_file=$WORK_DIR/redactions
printf '%s\n' "$password" > "$redactions_file"

sync_fixture() {
  find "$fixture" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
  remote_docker exec "$container_id" tar -C /upgrade-e2e/fixture -cf - . | tar -C "$fixture" -xf -
  cat "$fixture/server_private.key" "$fixture/preshared.key" \
    "$fixture/admin_private.key" "$fixture/user_private.key" >> "$redactions_file"
}

run_scenario() {
  local scenario=$1
  local log_file=$ARTIFACT_DIR/$scenario.log
  : > "$log_file"
  chmod 0600 "$log_file"
  remote_docker exec "$container_id" /usr/local/bin/upgrade-e2e-seed "$nonce"
  sync_fixture
  remote_docker exec "$container_id" /usr/local/bin/upgrade-e2e-probe-peers \
    "$nonce" "${scenario}-before" 2>&1 | tee -a "$log_file"
  case "$scenario" in
    rollback)
      remote_docker exec "$container_id" /usr/local/bin/upgrade-e2e-enable-conflict "$nonce"
      ;;
    recovery)
      remote_docker exec "$container_id" /usr/local/bin/upgrade-e2e-prepare-recovery "$nonce"
      ;;
  esac
  local expected_state=active
  if [ "$scenario" = recovery ]; then
    expected_state=recovery
  fi
  remote_docker exec "$container_id" /usr/local/bin/upgrade-e2e-attest \
    "$nonce" "$expected_state" 2>&1 | tee -a "$log_file"
  UPGRADE_E2E_EXECUTION=container-lab \
  UPGRADE_E2E_DISPOSABLE=1 \
  UPGRADE_E2E_LAB_NONCE="$nonce" \
  UPGRADE_E2E_HOST=127.0.0.1 \
  UPGRADE_E2E_PORT="$local_port" \
  UPGRADE_E2E_USER=amnezia-e2e \
  UPGRADE_E2E_PASSWORD="$password" \
  UPGRADE_E2E_FIXTURE_DIR="$fixture" \
    nice -n 15 "$BINARY" "$scenario" 2>&1 | tee -a "$log_file"
  for native_config in upgraded-admin.conf reissued-user.conf; do
    if [ -f "$fixture/$native_config" ]; then
      remote_docker exec -i "$container_id" sh -c \
        'umask 077; cat > "/upgrade-e2e/fixture/$1"' sh "$native_config" \
        < "$fixture/$native_config"
    fi
  done
  if [ -f "$fixture/reissued-user-private.key" ]; then
    cat "$fixture/reissued-user-private.key" >> "$redactions_file"
  fi
  remote_docker exec "$container_id" /usr/local/bin/upgrade-e2e-probe-peers \
    "$nonce" "${scenario}-after" 2>&1 | tee -a "$log_file"
}

run_scenario refresh
run_scenario protocol
run_scenario reissue
run_scenario rollback
run_scenario recovery

remote_docker exec "$container_id" cat /var/log/upgrade-e2e-dockerd.log > "$ARTIFACT_DIR/dockerd.log"
remote_docker exec "$container_id" cat /run/amnezia-upgrade-e2e-awg-base.manifest \
  > "$ARTIFACT_DIR/awg-base.manifest"
chmod 0600 "$ARTIFACT_DIR/dockerd.log" "$ARTIFACT_DIR/awg-base.manifest"
redact_artifacts
printf '%s\n' "All isolated container upgrade scenarios passed"
