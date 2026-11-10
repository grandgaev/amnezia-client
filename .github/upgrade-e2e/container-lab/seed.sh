#!/bin/bash
set -euo pipefail

nonce=${1:-}
real_docker=/usr/bin/docker
fixture=/upgrade-e2e/fixture
container=amnezia-awg2
parked=amnezia-awg2-pre-upgrade
image=amnezia-awg2
label=com.amnezia.upgrade-e2e.run
umask 077

test -f /.dockerenv
test "$nonce" = "$(cat /run/amnezia-upgrade-e2e-lab)"
test "$nonce" = "$(cat /opt/amnezia/.upgrade-e2e-owner)"

remove_owned_container() {
  local name=$1
  if "$real_docker" container inspect "$name" >/dev/null 2>&1; then
    test "$("$real_docker" inspect -f '{{ index .Config.Labels "com.amnezia.upgrade-e2e.run" }}' "$name")" = "$nonce"
    "$real_docker" rm -f "$name" >/dev/null
  fi
}

remove_owned_container "$container"
remove_owned_container "$parked"
if "$real_docker" network inspect amnezia-dns-net >/dev/null 2>&1; then
  test "$("$real_docker" network inspect -f '{{ index .Labels "com.amnezia.upgrade-e2e.run" }}' amnezia-dns-net)" = "$nonce"
  "$real_docker" network rm amnezia-dns-net >/dev/null
fi
if "$real_docker" image inspect "$image" >/dev/null 2>&1; then
  test "$("$real_docker" image inspect -f '{{ index .Config.Labels "com.amnezia.upgrade-e2e.run" }}' "$image")" = "$nonce"
  "$real_docker" image rm -f "$image" >/dev/null
fi

rm -rf /opt/amnezia/awg
install -d -m 0755 "$fixture"
find "$fixture" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +

"$real_docker" network create \
  --label "$label=$nonce" \
  --driver bridge \
  --subnet=172.29.172.0/24 \
  --opt com.docker.network.bridge.name=amn0 \
  amnezia-dns-net >/dev/null
/opt/amnezia-e2e/bin/docker build \
  -t "$image" /workspace/client/server_scripts/awg >/dev/null

keygen() {
  "$real_docker" run --rm --entrypoint awg "$image" genkey
}
pubkey() {
  "$real_docker" run --rm -i --entrypoint awg "$image" pubkey
}
pskgen() {
  "$real_docker" run --rm --entrypoint awg "$image" genpsk
}

keygen > "$fixture/server_private.key"
pubkey < "$fixture/server_private.key" > "$fixture/server_public.key"
pskgen > "$fixture/preshared.key"
keygen > "$fixture/admin_private.key"
pubkey < "$fixture/admin_private.key" > "$fixture/admin_public.key"
keygen > "$fixture/user_private.key"
pubkey < "$fixture/user_private.key" > "$fixture/user_public.key"

server_private=$(tr -d '\r\n' < "$fixture/server_private.key")
server_public=$(tr -d '\r\n' < "$fixture/server_public.key")
preshared=$(tr -d '\r\n' < "$fixture/preshared.key")
admin_private=$(tr -d '\r\n' < "$fixture/admin_private.key")
admin_public=$(tr -d '\r\n' < "$fixture/admin_public.key")
user_public=$(tr -d '\r\n' < "$fixture/user_public.key")

cat > "$fixture/awg0.conf" <<EOF
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

cat > "$fixture/admin.conf" <<EOF
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

cat > "$fixture/clientsTable" <<EOF
[
  {"clientId":"$admin_public","userData":{"clientName":"Administrator"}},
  {"clientId":"$user_public","userData":{"clientName":"Preserved user"}}
]
EOF

# The source placeholders must remain literal for sed.
# shellcheck disable=SC2016
sed \
  -e 's/$AWG_SUBNET_IP/10.8.1.1/g' \
  -e 's/$WIREGUARD_SUBNET_CIDR/24/g' \
  /workspace/client/server_scripts/awg/start.sh > "$fixture/start.sh"

"$real_docker" run -d \
  --label "$label=$nonce" \
  --log-driver none \
  --restart always \
  --cap-drop=ALL \
  --cap-add=NET_ADMIN \
  --device=/dev/net/tun \
  --sysctl net.ipv4.conf.all.src_valid_mark=1 \
  -p 51820:51820/udp \
  --name "$container" \
  "$image" >/dev/null
"$real_docker" network connect amnezia-dns-net "$container"

for path in awg0.conf server_private.key server_public.key preshared.key clientsTable; do
  case "$path" in
    server_private.key) target=/opt/amnezia/awg/wireguard_server_private_key.key ;;
    server_public.key) target=/opt/amnezia/awg/wireguard_server_public_key.key ;;
    preshared.key) target=/opt/amnezia/awg/wireguard_psk.key ;;
    *) target="/opt/amnezia/awg/$path" ;;
  esac
  "$real_docker" exec "$container" mkdir -p "$(dirname "$target")"
  "$real_docker" cp "$fixture/$path" "$container:$target"
done
"$real_docker" cp "$fixture/start.sh" "$container:/opt/amnezia/start.sh"
"$real_docker" exec "$container" chmod 0755 /opt/amnezia/start.sh
"$real_docker" exec "$container" awg-quick up /opt/amnezia/awg/awg0.conf

"$real_docker" inspect -f '{{.State.Running}}' "$container" | grep -qx true
"$real_docker" exec "$container" awg show awg0 peers | grep -c . | grep -qx 2
if find "$fixture" -mindepth 1 -maxdepth 1 \( ! -user root -o ! -group root \) -print -quit | grep -q .; then
  exit 1
fi
