#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# shellcheck source=.github/upgrade-e2e/guard.sh
source "$ROOT/.github/upgrade-e2e/guard.sh"
upgrade_e2e_guard

E2E_USER=${UPGRADE_E2E_USER:-amnezia-e2e}
E2E_PASSWORD=${UPGRADE_E2E_PASSWORD:-amnezia-e2e-password}
E2E_PORT=${UPGRADE_E2E_PORT:-2222}

if sudo test -e "$UPGRADE_E2E_OWNER_MARKER" || sudo test -L "$UPGRADE_E2E_OWNER_MARKER"; then
  echo "Upgrade e2e ownership marker already exists" >&2
  exit 1
fi
if id "$E2E_USER" >/dev/null 2>&1 || sudo test -e "/etc/sudoers.d/$E2E_USER" \
  || sudo test -L "/etc/sudoers.d/$E2E_USER"; then
  echo "Upgrade e2e account already exists" >&2
  exit 1
fi
if sudo test -e /opt/amnezia || sudo test -L /opt/amnezia \
  || sudo test -e /opt/amnezia-e2e || sudo test -L /opt/amnezia-e2e; then
  echo "Existing Amnezia data under /opt prevents the disposable upgrade test" >&2
  exit 1
fi
if [ -n "$(sudo docker ps -aq)" ]; then
  echo "Existing Docker containers prevent the disposable upgrade test" >&2
  exit 1
fi
if sudo docker network inspect amnezia-dns-net >/dev/null 2>&1; then
  echo "Existing amnezia-dns-net network prevents the disposable upgrade test" >&2
  exit 1
fi
if sudo docker image inspect amnezia-awg2 >/dev/null 2>&1; then
  echo "Existing amnezia-awg2 image prevents the disposable upgrade test" >&2
  exit 1
fi
if command -v ss >/dev/null 2>&1 && ss -H -ltn "sport = :$E2E_PORT" | grep -q .; then
  echo "Upgrade e2e SSH port is already in use" >&2
  exit 1
fi

printf '%s\n' "$UPGRADE_E2E_RUN_ID" | sudo tee "$UPGRADE_E2E_OWNER_MARKER" >/dev/null
sudo chmod 0600 "$UPGRADE_E2E_OWNER_MARKER"

sudo apt-get update
sudo apt-get install --no-install-recommends -y openssh-server sudo

sudo useradd --create-home --shell /bin/bash --groups sudo "$E2E_USER"
printf '%s:%s\n' "$E2E_USER" "$E2E_PASSWORD" | sudo chpasswd
printf '%s ALL=(ALL) NOPASSWD: ALL\n' "$E2E_USER" |
  sudo tee "/etc/sudoers.d/$E2E_USER" >/dev/null
printf 'Defaults:%s secure_path="/opt/amnezia-e2e/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"\n' "$E2E_USER" |
  sudo tee -a "/etc/sudoers.d/$E2E_USER" >/dev/null
sudo chmod 0440 "/etc/sudoers.d/$E2E_USER"

sudo install -d -m 0755 /run/sshd
sudo ssh-keygen -A
sudo sed -i \
  -e 's/^#\?PasswordAuthentication .*/PasswordAuthentication yes/' \
  -e 's/^#\?KbdInteractiveAuthentication .*/KbdInteractiveAuthentication no/' \
  /etc/ssh/sshd_config
sudo /usr/sbin/sshd -p "$E2E_PORT"

for _ in $(seq 1 50); do
  if nc -z 127.0.0.1 "$E2E_PORT"; then
    exit 0
  fi
  sleep 0.1
done
echo "Disposable SSH server did not start" >&2
exit 1
