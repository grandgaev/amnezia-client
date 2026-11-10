#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  exit 2
fi

work_dir=$(mktemp -d)
bus_pid=""

cleanup() {
  if [[ "$bus_pid" =~ ^[0-9]+$ ]]; then
    kill "$bus_pid" 2>/dev/null || true
    wait "$bus_pid" 2>/dev/null || true
  fi
  rm -rf "$work_dir"
}
trap cleanup EXIT INT TERM

bus_output=$(dbus-daemon --session --fork \
  --address="unix:path=$work_dir/system-bus" \
  --print-address=1 --print-pid=1)
bus_address=$(printf '%s\n' "$bus_output" | sed -n '1p')
bus_pid=$(printf '%s\n' "$bus_output" | sed -n '2p')

if [[ "$bus_address" != "unix:path=$work_dir/system-bus"* ]] ||
   [[ ! "$bus_pid" =~ ^[0-9]+$ ]]; then
  exit 3
fi

export DBUS_SYSTEM_BUS_ADDRESS="$bus_address"
export AMNEZIA_FAKE_RESOLVE1="$2"
unset DBUS_SESSION_BUS_ADDRESS DBUS_STARTER_ADDRESS DBUS_STARTER_BUS_TYPE

"$1"
