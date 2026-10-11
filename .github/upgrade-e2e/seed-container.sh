#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# shellcheck source=.github/upgrade-e2e/guard.sh
source "$ROOT/.github/upgrade-e2e/guard.sh"
upgrade_e2e_guard
upgrade_e2e_require_owner

FIXTURE_DIR=${1:?fixture directory is required}
upgrade_e2e_require_runner_path "$FIXTURE_DIR"
CONTAINER=amnezia-awg2
PARKED=${CONTAINER}-pre-upgrade
IMAGE=amnezia-awg2

mkdir -p "$FIXTURE_DIR"
rm -f "$FIXTURE_DIR"/*
sudo docker rm -f "$CONTAINER" "$PARKED" >/dev/null 2>&1 || true
sudo rm -rf "/opt/amnezia/$CONTAINER"

if ! sudo docker network inspect amnezia-dns-net >/dev/null 2>&1; then
  sudo docker network create \
    --driver bridge \
    --subnet=172.29.172.0/24 \
    --opt com.docker.network.bridge.name=amn0 \
    amnezia-dns-net >/dev/null
fi

sudo docker build -t "$IMAGE" "$ROOT/client/server_scripts/awg"

keygen() {
  sudo docker run --rm --entrypoint awg "$IMAGE" genkey
}
pubkey() {
  sudo docker run --rm -i --entrypoint awg "$IMAGE" pubkey
}
pskgen() {
  sudo docker run --rm --entrypoint awg "$IMAGE" genpsk
}

keygen > "$FIXTURE_DIR/server_private.key"
pubkey < "$FIXTURE_DIR/server_private.key" > "$FIXTURE_DIR/server_public.key"
pskgen > "$FIXTURE_DIR/preshared.key"
keygen > "$FIXTURE_DIR/admin_private.key"
pubkey < "$FIXTURE_DIR/admin_private.key" > "$FIXTURE_DIR/admin_public.key"
keygen > "$FIXTURE_DIR/user_private.key"
pubkey < "$FIXTURE_DIR/user_private.key" > "$FIXTURE_DIR/user_public.key"

server_private=$(tr -d '\r\n' < "$FIXTURE_DIR/server_private.key")
server_public=$(tr -d '\r\n' < "$FIXTURE_DIR/server_public.key")
preshared=$(tr -d '\r\n' < "$FIXTURE_DIR/preshared.key")
admin_private=$(tr -d '\r\n' < "$FIXTURE_DIR/admin_private.key")
admin_public=$(tr -d '\r\n' < "$FIXTURE_DIR/admin_public.key")
user_public=$(tr -d '\r\n' < "$FIXTURE_DIR/user_public.key")

cat > "$FIXTURE_DIR/awg0.conf" <<EOF
[Interface]
PrivateKey = $server_private
Address = 10.8.1.1/24
ListenPort = 51820
Jc = 5
Jmin = 10
Jmax = 50
S1 = 24
S2 = 64
S3 = 18
S4 = 7
H1 = 100000
H2 = 300000
H3 = 500000
H4 = 700000

[Peer]
PublicKey = $admin_public
PresharedKey = $preshared
AllowedIPs = 10.8.1.2/32

[Peer]
PublicKey = $user_public
PresharedKey = $preshared
AllowedIPs = 10.8.1.3/32
EOF

cat > "$FIXTURE_DIR/admin.conf" <<EOF
[Interface]
PrivateKey = $admin_private
Address = 10.8.1.2/32
DNS = 1.1.1.1

[Peer]
PublicKey = $server_public
PresharedKey = $preshared
Endpoint = 127.0.0.1:51820
AllowedIPs = 0.0.0.0/0, ::/0
PersistentKeepalive = 25
EOF

cat > "$FIXTURE_DIR/clientsTable" <<'EOF'
[
  {"clientId":"admin-client","userData":{"clientName":"Administrator"}},
  {"clientId":"user-client","userData":{"clientName":"Preserved user"}}
]
EOF

# The app template placeholders are literal shell variables at this point.
# shellcheck disable=SC2016
sed \
  -e 's/$AWG_SUBNET_IP/10.8.1.1/g' \
  -e 's/$WIREGUARD_SUBNET_CIDR/24/g' \
  "$ROOT/client/server_scripts/awg/start.sh" > "$FIXTURE_DIR/start.sh"

export CONTAINER_NAME=$CONTAINER
export AWG_SERVER_PORT=51820
export AWG_SUBNET_IP=10.8.1.1
export WIREGUARD_SUBNET_CIDR=24
bash "$ROOT/client/server_scripts/awg/run_container.sh"

for path in awg0.conf server_private.key server_public.key preshared.key clientsTable; do
  case "$path" in
    server_private.key) target=/opt/amnezia/awg/wireguard_server_private_key.key ;;
    server_public.key) target=/opt/amnezia/awg/wireguard_server_public_key.key ;;
    preshared.key) target=/opt/amnezia/awg/wireguard_psk.key ;;
    *) target="/opt/amnezia/awg/$path" ;;
  esac
  sudo docker exec "$CONTAINER" mkdir -p "$(dirname "$target")"
  sudo docker cp "$FIXTURE_DIR/$path" "$CONTAINER:$target"
done
sudo docker cp "$FIXTURE_DIR/start.sh" "$CONTAINER:/opt/amnezia/start.sh"
sudo docker exec "$CONTAINER" chmod 0755 /opt/amnezia/start.sh
sudo docker exec "$CONTAINER" awg-quick up /opt/amnezia/awg/awg0.conf

sudo docker inspect -f '{{.State.Running}}' "$CONTAINER" | grep -qx true
sudo docker exec "$CONTAINER" awg show awg0 peers | grep -c . | grep -qx 2
