#!/bin/bash
set -euo pipefail

attestation_criterion=arguments
attestation_error() {
  printf 'attestation failed: line=%s criterion=%s\n' \
    "$2" "$attestation_criterion" >&2
  exit "$1"
}
trap 'attestation_error "$?" "$LINENO"' ERR

expected_nonce=${1:-}
expected_state=${2:-}
case "$expected_nonce" in
  ''|*[!0-9a-f]*) false ;;
esac
case "$expected_state" in
  active|recovery) ;;
  *) false ;;
esac

attestation_criterion=lab-boundary
test -f /.dockerenv
test "$(/usr/bin/docker info --format '{{.Driver}}')" = vfs
test "$(stat -c '%U:%G:%a' /run/amnezia-upgrade-e2e-lab)" = root:root:600
test "$(stat -c '%U:%G:%a' /opt/amnezia/.upgrade-e2e-owner)" = root:root:600
test "$(cat /run/amnezia-upgrade-e2e-lab)" = "$expected_nonce"
test "$(cat /opt/amnezia/.upgrade-e2e-owner)" = "$expected_nonce"
test "$(stat -c '%U:%G:%a' /opt/amnezia-e2e/bin/docker)" = root:root:755
test "$(stat -c '%U:%G:%a' /opt/amnezia-e2e/bin/sysctl)" = root:root:755

attestation_criterion=base-provenance
test "$(stat -c '%U:%G:%a' /run/amnezia-upgrade-e2e-awg-base)" = root:root:600
test "$(stat -c '%U:%G:%a' /run/amnezia-upgrade-e2e-awg-base.manifest)" = root:root:600
grep -Eq '^amneziavpn/amneziawg-go@sha256:[0-9a-f]{64}$' /run/amnezia-upgrade-e2e-awg-base

attestation_criterion=fixture-ownership
test "$(stat -c '%U:%G' /upgrade-e2e/fixture)" = root:root
if find /upgrade-e2e/fixture -mindepth 1 -maxdepth 1 \( ! -user root -o ! -group root \) -print -quit | grep -q .; then
  false
fi

attestation_criterion=container-inventory
mapfile -t container_ids < <(/usr/bin/docker ps -aq)
test "${#container_ids[@]}" -eq 1
container_id=${container_ids[0]}

attestation_criterion=container-ownership
test "$(/usr/bin/docker inspect -f '{{ index .Config.Labels "com.amnezia.upgrade-e2e.run" }}' "$container_id")" = "$expected_nonce"

attestation_criterion=container-security
test "$(/usr/bin/docker inspect -f '{{.HostConfig.Privileged}}' "$container_id")" = false
if /usr/bin/docker inspect -f '{{json .HostConfig.CapAdd}}' "$container_id" | grep -q SYS_MODULE; then
  false
fi
case "$(/usr/bin/docker inspect -f '{{range .HostConfig.CapAdd}}{{println .}}{{end}}' "$container_id")" in
  NET_ADMIN|CAP_NET_ADMIN) ;;
  *) false ;;
esac
case "$(/usr/bin/docker inspect -f '{{range .HostConfig.CapDrop}}{{println .}}{{end}}' "$container_id")" in
  ALL|CAP_ALL) ;;
  *) false ;;
esac
test "$(/usr/bin/docker inspect -f '{{range .HostConfig.Devices}}{{println .PathOnHost .PathInContainer .CgroupPermissions}}{{end}}' "$container_id")" \
  = '/dev/net/tun /dev/net/tun rwm'
if /usr/bin/docker inspect -f '{{json .HostConfig.Binds}}' "$container_id" | grep -q '/lib/modules'; then
  false
fi
case "$expected_state" in
  active)
    attestation_criterion=active-runtime
    test "$(/usr/bin/docker inspect -f '{{.Name}}' "$container_id")" = /amnezia-awg2
    test "$(/usr/bin/docker inspect -f '{{.State.Running}}' "$container_id")" = true
    test "$(/usr/bin/docker exec "$container_id" sh -c 'command -v ip')" = /usr/local/bin/ip
    /usr/bin/docker exec "$container_id" grep -Fq 'type amneziawg' /usr/local/bin/ip
    /usr/bin/docker exec "$container_id" grep -Fq '.amnezia-real' /usr/local/bin/ip
    /usr/bin/docker exec "$container_id" sh -c 'pidof amneziawg-go >/dev/null'
    ;;
  recovery)
    attestation_criterion=recovery-runtime
    test "$(/usr/bin/docker inspect -f '{{.Name}}' "$container_id")" = /amnezia-awg2-pre-upgrade
    ;;
esac

attestation_criterion=network-ownership
test "$(/usr/bin/docker network inspect -f '{{ index .Labels "com.amnezia.upgrade-e2e.run" }}' amnezia-dns-net)" = "$expected_nonce"

attestation_criterion=image-ownership
test "$(/usr/bin/docker image inspect -f '{{ index .Config.Labels "com.amnezia.upgrade-e2e.run" }}' amnezia-awg2)" = "$expected_nonce"
test "$(/usr/bin/docker image inspect -f '{{ index .Config.Labels "com.amnezia.upgrade-e2e.base" }}' amnezia-awg2)" \
  = "$(cat /run/amnezia-upgrade-e2e-awg-base)"
test "$(/usr/bin/docker image inspect -f '{{ index .Config.Labels "com.amnezia.upgrade-e2e.userspace" }}' amnezia-awg2)" \
  = forced

attestation_criterion=userspace-image
/usr/bin/docker image inspect -f '{{range .Config.Env}}{{println .}}{{end}}' amnezia-awg2 |
  grep -qx 'WG_QUICK_USERSPACE_IMPLEMENTATION=amneziawg-go'
/usr/bin/docker image inspect -f '{{range .Config.Env}}{{println .}}{{end}}' amnezia-awg2 |
  grep -qx 'PATH=/usr/local/bin:/usr/local/sbin:/usr/sbin:/usr/bin:/sbin:/bin'
printf '%s\n' OK
