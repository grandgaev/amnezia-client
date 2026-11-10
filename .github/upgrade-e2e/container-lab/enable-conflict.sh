#!/bin/bash
set -euo pipefail

nonce=${1:-}
test -f /.dockerenv
test "$nonce" = "$(cat /run/amnezia-upgrade-e2e-lab)"
test "$nonce" = "$(cat /opt/amnezia/.upgrade-e2e-owner)"
/usr/bin/docker pull alpine:3.20 >/dev/null
touch /opt/amnezia-e2e/inject-docker-name-conflict
