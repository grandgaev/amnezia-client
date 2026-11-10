#!/bin/bash
set -euo pipefail

nonce=${1:-}
phase=${2:-}
real_docker=/usr/bin/docker
fixture=/upgrade-e2e/fixture
server=amnezia-awg2
network=amnezia-dns-net
image=amnezia-awg2
owner_label=com.amnezia.upgrade-e2e.run
role_label=com.amnezia.upgrade-e2e.role
phase_label=com.amnezia.upgrade-e2e.phase
client_ids=()
temp_files=()

case "$nonce" in
  ''|*[!0-9a-f]*) exit 1 ;;
esac
if [ "${#nonce}" -lt 32 ] || [ "${#nonce}" -gt 64 ]; then
  exit 1
fi
case "$phase" in
  refresh-before|refresh-after|protocol-before|protocol-after|\
  reissue-before|reissue-after|rollback-before|rollback-after|recovery-before|recovery-after) ;;
  *) exit 1 ;;
esac

cleanup() {
  local status=$?
  local id
  local owned
  trap - EXIT INT TERM
  for id in "${client_ids[@]}"; do
    if ! "$real_docker" container inspect "$id" >/dev/null 2>&1; then
      continue
    fi
    owned=$(
      "$real_docker" inspect \
        -f '{{ index .Config.Labels "com.amnezia.upgrade-e2e.run" }} {{ index .Config.Labels "com.amnezia.upgrade-e2e.role" }}' \
        "$id"
    )
    if [ "$owned" != "$nonce peer-probe" ]; then
      printf '%s\n' "Refusing to remove a peer probe container with mismatched ownership" >&2
      status=1
      continue
    fi
    "$real_docker" rm -f "$id" >/dev/null || status=1
  done
  if [ "${#temp_files[@]}" -gt 0 ]; then
    rm -f -- "${temp_files[@]}"
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

test -f /.dockerenv
test "$nonce" = "$(cat /run/amnezia-upgrade-e2e-lab)"
test "$nonce" = "$(cat /opt/amnezia/.upgrade-e2e-owner)"
test "$("$real_docker" inspect -f '{{ index .Config.Labels "com.amnezia.upgrade-e2e.run" }}' "$server")" = "$nonce"
test "$("$real_docker" inspect -f '{{.State.Running}}' "$server")" = true
test "$("$real_docker" network inspect -f '{{ index .Labels "com.amnezia.upgrade-e2e.run" }}' "$network")" = "$nonce"
test "$("$real_docker" image inspect -f '{{ index .Config.Labels "com.amnezia.upgrade-e2e.run" }}' "$image")" = "$nonce"

for secret in admin_private.key user_private.key preshared.key; do
  test -f "$fixture/$secret"
  test "$(stat -c '%U:%G' "$fixture/$secret")" = root:root
  mode=$(stat -c '%a' "$fixture/$secret")
  test $((8#$mode & 077)) -eq 0
done
for public in server_public.key admin_public.key user_public.key; do
  test -f "$fixture/$public"
  test "$(stat -c '%U:%G' "$fixture/$public")" = root:root
done

server_ip=$("$real_docker" inspect -f '{{ (index .NetworkSettings.Networks "amnezia-dns-net").IPAddress }}' "$server")
case "$server_ip" in
  172.29.172.*) ;;
  *) exit 1 ;;
esac

# shellcheck disable=SC2016
listen_port=$(
  "$real_docker" exec "$server" awk '
    $0 == "[Interface]" { interface = 1; next }
    interface && $0 ~ /^\[Peer\]/ { exit }
    interface && $1 == "ListenPort" && $2 == "=" { print $3; found = 1 }
    END { if (!found) exit 1 }
  ' /opt/amnezia/awg/awg0.conf
)
case "$listen_port" in
  ''|*[!0-9]*) exit 1 ;;
esac
test "$listen_port" -ge 1
test "$listen_port" -le 65535

# shellcheck disable=SC2016
transport_params=$(
  "$real_docker" exec "$server" awk '
    function trim(value) {
      sub(/^[[:space:]]+/, "", value)
      sub(/[[:space:]]+$/, "", value)
      return value
    }
    $0 == "[Interface]" { interface = 1; next }
    interface && $0 ~ /^\[Peer\]/ { exit }
    !interface || $0 ~ /^[[:space:]]*(#|$)/ { next }
    {
      separator = index($0, "=")
      if (!separator) exit 20
      key = trim(substr($0, 1, separator - 1))
      value = trim(substr($0, separator + 1))
      if (key == "PrivateKey" || key == "Address" || key == "ListenPort") next
      if (key !~ /^(Jc|Jmin|Jmax|JMin|JMax|S[1-4]|H[1-4]|I[1-5]|HeaderProtectionKey|ContentPaddingAddition|RekeyAfterTime|RekeyTimeout|RejectAfterTime|KeepaliveTimeout|MaxHandshakeAttempts|RandomTrailers|DisableCookies)$/) exit 21
      if (value == "") exit 22
      print key " = " value
      found = 1
    }
    END { if (!found) exit 23 }
  ' /opt/amnezia/awg/awg0.conf
)

probe_peer() {
  local peer=$1
  local address=$2
  local private_key_file=$3
  local name="amn-peer-probe-${peer}-${nonce:0:12}"
  local config
  local native_config=
  local id

  if "$real_docker" container inspect "$name" >/dev/null 2>&1; then
    printf '%s\n' "Peer probe container name is already in use" >&2
    return 1
  fi

  config=$(mktemp "$fixture/.peer-probe-${peer}.XXXXXX")
  temp_files+=("$config")
  chmod 0600 "$config"
  case "$phase:$peer" in
    protocol-after:admin|reissue-after:admin)
      native_config=$fixture/upgraded-admin.conf
      ;;
    reissue-after:preserved-user)
      native_config=$fixture/reissued-user.conf
      ;;
  esac
  if [ -n "$native_config" ]; then
    test "$(stat -c '%U:%G:%a' "$native_config")" = root:root:600
    awk -v endpoint="$server_ip:$listen_port" '
      /^[[:space:]]*DNS[[:space:]]*=/ { next }
      /^[[:space:]]*Endpoint[[:space:]]*=/ {
        print "Endpoint = " endpoint
        endpoints++
        next
      }
      { print }
      END { if (endpoints != 1) exit 1 }
    ' "$native_config" > "$config"
  else
    {
    printf '%s\n' '[Interface]'
    printf 'PrivateKey = %s\n' "$(tr -d '\r\n' < "$fixture/$private_key_file")"
    printf 'Address = %s/32\n' "$address"
    printf '%s\n' "$transport_params"
    printf '\n%s\n' '[Peer]'
    printf 'PublicKey = %s\n' "$(tr -d '\r\n' < "$fixture/server_public.key")"
    printf 'PresharedKey = %s\n' "$(tr -d '\r\n' < "$fixture/preshared.key")"
    printf 'Endpoint = %s:%s\n' "$server_ip" "$listen_port"
    printf '%s\n' 'AllowedIPs = 10.8.1.1/32'
    printf '%s\n' 'PersistentKeepalive = 5'
    } > "$config"
  fi

  # shellcheck disable=SC2016
  id=$(
    "$real_docker" create \
      --name "$name" \
      --label "$owner_label=$nonce" \
      --label "$role_label=peer-probe" \
      --label "$phase_label=$phase" \
      --network "$network" \
      --cap-drop ALL \
      --cap-add NET_ADMIN \
      --cap-add NET_RAW \
      --device /dev/net/tun \
      --security-opt no-new-privileges \
      --read-only \
      --tmpfs /run:rw,nosuid,nodev,noexec,size=8m \
      --tmpfs /tmp:rw,nosuid,nodev,noexec,size=8m \
      --log-driver none \
      --entrypoint /bin/sh \
      "$image" \
      -c 'set -eu; mkdir -m 0700 /run/amnezia-probe; touch /run/amnezia-probe/ready; exec sleep 120'
  )
  client_ids+=("$id")
  "$real_docker" start "$id" >/dev/null
  for attempt in 1 2 3 4 5; do
    if "$real_docker" exec "$id" test -f /run/amnezia-probe/ready; then
      break
    fi
    if [ "$attempt" -eq 5 ]; then
      printf 'Peer probe container did not become ready for %s during %s\n' "$peer" "$phase" >&2
      return 1
    fi
    sleep 1
  done
  "$real_docker" exec -i "$id" /bin/sh -c \
    'umask 077; cat > /run/amnezia-probe/probe.conf' < "$config"
  rm -f "$config"

  # shellcheck disable=SC2016
  if ! timeout 45 "$real_docker" exec "$id" /bin/bash -c '
        set -euo pipefail
        config=/run/amnezia-probe/probe.conf
        down() {
          ip link delete dev probe >/dev/null 2>&1 || true
          rm -f /var/run/amneziawg/probe.sock
        }
        trap down EXIT INT TERM
        amneziawg-go probe >/run/amnezia-probe/amneziawg-go.log 2>&1
        for attempt in 1 2 3 4 5; do
          ip link show dev probe >/dev/null 2>&1 && break
          test "$attempt" -lt 5
          sleep 1
        done
        awg setconf probe <(awg-quick strip "$config")
        address=$(sed -n "s/^[[:space:]]*Address[[:space:]]*=[[:space:]]*//p" "$config")
        test -n "$address"
        test "$(printf "%s\n" "$address" | wc -l | tr -d " ")" -eq 1
        ip -4 address add "$address" dev probe
        ip link set mtu 1376 up dev probe
        ip -4 route add 10.8.1.1/32 dev probe
        reached=
        for attempt in 1 2 3 4 5; do
          if ping -c 1 -W 2 10.8.1.1 >/dev/null 2>&1; then
            reached=1
            break
          fi
          sleep 1
        done
        test "$reached" = 1
        handshake=$(awg show probe latest-handshakes)
        set -- $handshake
        test "$#" -eq 2
        latest=$2
        case "$latest" in ""|*[!0-9]*) exit 31 ;; esac
        now=$(date +%s)
        age=$((now - latest))
        test "$age" -ge 0
        test "$age" -le 30
      ' >/dev/null; then
    printf 'Peer probe failed for %s during %s\n' "$peer" "$phase" >&2
    return 1
  fi
  "$real_docker" rm -f "$id" >/dev/null
}

probe_peer admin 10.8.1.2 admin_private.key
probe_peer preserved-user 10.8.1.3 user_private.key
printf 'Peer probes passed during %s\n' "$phase"
