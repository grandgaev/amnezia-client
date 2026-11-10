#!/bin/bash
set -euo pipefail

nonce=${UPGRADE_E2E_LAB_NONCE:-}
password=${UPGRADE_E2E_PASSWORD:-}
case "$nonce" in
  ''|*[!0-9a-f]*)
    echo "Container lab nonce is invalid" >&2
    exit 1
    ;;
esac
if [ "${#nonce}" -lt 32 ] || [ "${#nonce}" -gt 64 ] || [ -z "$password" ]; then
  echo "Container lab credentials are invalid" >&2
  exit 1
fi

useradd --create-home --shell /bin/bash --groups sudo amnezia-e2e
printf '%s:%s\n' amnezia-e2e "$password" | chpasswd
printf '%s\n' 'amnezia-e2e ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/amnezia-e2e
printf '%s\n' 'Defaults:amnezia-e2e secure_path="/opt/amnezia-e2e/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"' \
  >> /etc/sudoers.d/amnezia-e2e
chmod 0440 /etc/sudoers.d/amnezia-e2e

install -d -m 0755 /run/sshd /opt/amnezia /upgrade-e2e/fixture
printf '%s\n' "$nonce" > /run/amnezia-upgrade-e2e-lab
printf '%s\n' "$nonce" > /opt/amnezia/.upgrade-e2e-owner
chmod 0600 /run/amnezia-upgrade-e2e-lab /opt/amnezia/.upgrade-e2e-owner
chown root:root /run/amnezia-upgrade-e2e-lab /opt/amnezia/.upgrade-e2e-owner

ssh-keygen -A
cat > /etc/ssh/sshd_config.d/upgrade-e2e.conf <<'EOF'
PasswordAuthentication yes
KbdInteractiveAuthentication no
PermitRootLogin no
AllowUsers amnezia-e2e
EOF

dockerd --host=unix:///var/run/docker.sock > /var/log/upgrade-e2e-dockerd.log 2>&1 &
dockerd_pid=$!
for _ in $(seq 1 60); do
  if /usr/bin/docker info >/dev/null 2>&1; then
    if [ "$(/usr/bin/docker info --format '{{.Driver}}')" != vfs ]; then
      /usr/bin/docker info >&2
      exit 1
    fi
    /usr/local/bin/upgrade-e2e-pin-awg-base
    exec /usr/sbin/sshd -D -e
  fi
  if ! kill -0 "$dockerd_pid" 2>/dev/null; then
    cat /var/log/upgrade-e2e-dockerd.log >&2
    exit 1
  fi
  sleep 1
done
cat /var/log/upgrade-e2e-dockerd.log >&2
exit 1
