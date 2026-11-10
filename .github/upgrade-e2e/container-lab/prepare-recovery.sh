#!/bin/bash
set -euo pipefail

nonce=${1:-}
test -f /.dockerenv
test "$nonce" = "$(cat /run/amnezia-upgrade-e2e-lab)"
test "$nonce" = "$(cat /opt/amnezia/.upgrade-e2e-owner)"
test "$(/usr/bin/docker inspect -f '{{ index .Config.Labels "com.amnezia.upgrade-e2e.run" }}' amnezia-awg2)" = "$nonce"
/usr/bin/docker update --restart=no amnezia-awg2 >/dev/null
/usr/bin/docker stop amnezia-awg2 >/dev/null
/usr/bin/docker rename amnezia-awg2 amnezia-awg2-pre-upgrade
