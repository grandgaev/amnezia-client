#!/bin/bash
# End-to-end test of the amneziawg-go router in Linux network namespaces.
set -u -o pipefail

D=$(cd "$(dirname "$0")" && pwd)
AWG=${AWG_BIN:-$D/amneziawg-go}
H=${E2E_HELPER_BIN:-$D/e2ehelper}
RUN_SUFFIX=$(printf '%x' $$)
RUN_SUFFIX=${RUN_SUFFIX: -6}
NS_CLI="awg-cli-$RUN_SUFFIX"
NS_INET="awg-inet-$RUN_SUFFIX"
HCLI="hc$RUN_SUFFIX"
VCLI="vc$RUN_SUFFIX"
HINET="hi$RUN_SUFFIX"
INET_IF="in$RUN_SUFFIX"
AWGSRV="as$RUN_SUFFIX"
AWGCLI="ac$RUN_SUFFIX"
SOCKET_DIR=/var/run/amneziawg
SERVER_SOCKET="$SOCKET_DIR/$AWGSRV.sock"
CLIENT_SOCKET="$SOCKET_DIR/$AWGCLI.sock"
REMOVE_WORKDIR=0
if [ -n "${E2E_LOG_DIR:-}" ]; then
  WORKDIR=$E2E_LOG_DIR
  mkdir -p "$WORKDIR"
else
  WORKDIR=$(mktemp -d /tmp/amnezia-router-e2e.XXXXXX)
  REMOVE_WORKDIR=1
fi
ROUTING_CONFIG="$WORKDIR/routing.json"
ZERO_CONFIG="$WORKDIR/routing-zero.json"
PIDS=()
PASS=0
FAIL=0

ok() { echo "  PASS: $*"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $*"; FAIL=$((FAIL + 1)); }
check() {
  local want=$1 got=$2 what=$3
  if [ "$got" = "$want" ]; then
    ok "$what ($got)"
  else
    bad "$what: want '$want' got '$got'"
  fi
}

cleanup() {
  local pid
  for pid in "${PIDS[@]}"; do
    kill "$pid" 2>/dev/null || true
  done
  for pid in "${PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
  done
  ip netns del "$NS_CLI" 2>/dev/null || true
  ip netns del "$NS_INET" 2>/dev/null || true
  ip link del "$HCLI" 2>/dev/null || true
  ip link del "$HINET" 2>/dev/null || true
  rm -f "$SERVER_SOCKET" "$CLIENT_SOCKET"
  if [ "$REMOVE_WORKDIR" -eq 1 ]; then
    rm -rf "$WORKDIR"
  fi
}

write_state() {
  [ -n "${E2E_STATE_FILE:-}" ] || return 0
  {
    printf 'E2E_NS_CLI=%q\n' "$NS_CLI"
    printf 'E2E_NS_INET=%q\n' "$NS_INET"
    printf 'E2E_HCLI=%q\n' "$HCLI"
    printf 'E2E_HINET=%q\n' "$HINET"
    printf 'E2E_SERVER_SOCKET=%q\n' "$SERVER_SOCKET"
    printf 'E2E_CLIENT_SOCKET=%q\n' "$CLIENT_SOCKET"
    printf 'E2E_WORKDIR=%q\n' "$WORKDIR"
    printf 'E2E_PIDS=%q\n' "${PIDS[*]}"
  } > "$E2E_STATE_FILE"
}

trap cleanup EXIT
trap 'exit 130' INT TERM

for required in "$AWG" "$H"; do
  if [ ! -x "$required" ]; then
    echo "Missing executable: $required" >&2
    exit 1
  fi
done

ip netns add "$NS_CLI"
ip netns add "$NS_INET"
ip link add "$HCLI" type veth peer name "$VCLI"
ip link set "$VCLI" netns "$NS_CLI"
ip link add "$HINET" type veth peer name "$INET_IF"
ip link set "$INET_IF" netns "$NS_INET"
ip addr add 192.168.100.1/24 dev "$HCLI"
ip link set "$HCLI" up
ip addr add 172.31.255.1/24 dev "$HINET"
ip link set "$HINET" up
ip route add 172.31.0.0/16 via 172.31.255.2
sysctl -qw net.ipv4.ip_forward=1
ip -n "$NS_CLI" addr add 192.168.100.2/24 dev "$VCLI"
ip -n "$NS_CLI" link set "$VCLI" up
ip -n "$NS_CLI" link set lo up
ip -n "$NS_CLI" route add default via 192.168.100.1
ip -n "$NS_INET" addr add 172.31.255.2/24 dev "$INET_IF"
ip -n "$NS_INET" link set "$INET_IF" up
ip -n "$NS_INET" link set lo up
for address in 172.31.0.53 172.31.1.10 172.31.2.10 172.31.3.10 172.31.4.10 172.31.5.10; do
  ip -n "$NS_INET" addr add "$address/32" dev "$INET_IF"
done
ip -n "$NS_INET" route add default via 172.31.255.1

ip netns exec "$NS_INET" "$H" http \
  172.31.1.10:80 172.31.2.10:80 172.31.3.10:80 172.31.4.10:80 172.31.5.10:80 \
  >"$WORKDIR/http.log" 2>&1 &
PIDS+=("$!")
ip netns exec "$NS_INET" "$H" dns 172.31.0.53:53 \
  "direct.test=172.31.1.10,www.direct.test=172.31.1.10,proxy.test=172.31.2.10,other.test=172.31.4.10,ads.test=172.31.1.10,shared.test=172.31.1.10" \
  >"$WORKDIR/dns.log" 2>&1 &
PIDS+=("$!")

read -r SK SP _ < <("$H" keygen)
read -r CK CP _ < <("$H" keygen)
OBF_BASE="jc=4
jmin=10
jmax=50
s1=12
s2=18
s3=24
s4=32
h1=100-200
h2=300-400
h3=500-600
h4=700-800
header_protection_key=$(head -c32 /dev/urandom | od -An -tx1 | tr -d ' \n')
content_padding_addition=10-100
rekey_after_time=100-120
rekey_timeout=3-7
reject_after_time=150-180
keepalive_timeout=5-15
max_handshake_attempts=15-20
random_trailers=true
disable_cookies=true"
OBF=${OBF_OVERRIDE-$OBF_BASE}
[ -n "$OBF" ] || OBF="replace_peers=true"

mkdir -p "$SOCKET_DIR"
LOG_LEVEL=verbose WG_PROCESS_FOREGROUND=1 "$AWG" -f "$AWGSRV" >"$WORKDIR/server.log" 2>&1 &
PIDS+=("$!")
sleep 1
ip addr add 10.8.1.1/24 dev "$AWGSRV"
ip link set "$AWGSRV" up
printf "set=1\nprivate_key=%s\nlisten_port=51820\n%s\npublic_key=%s\nallowed_ip=10.8.1.2/32\n\n" \
  "$SK" "$OBF" "$CP" | "$H" uapi "$SERVER_SOCKET" | grep -q errno=0 || {
    echo "Server configuration failed" >&2
    exit 1
  }

cat > "$ROUTING_CONFIG" <<'EOF'
{
  "version": 1,
  "defaultAction": "proxy",
  "rules": [
    {"action": "block",  "domains": ["domain:ads.test"], "ips": ["172.31.5.10"]},
    {"action": "proxy",  "domains": ["domain:proxy.test", "domain:shared.test"]},
    {"action": "direct", "domains": ["domain:direct.test"], "ips": ["172.31.3.0/24"]}
  ],
  "dns": {"listen": ["198.18.0.53"], "remote": ["172.31.0.53"], "direct": ["172.31.0.53"]},
  "bypass": "interface"
}
EOF

ip netns exec "$NS_CLI" env LOG_LEVEL=verbose WG_PROCESS_FOREGROUND=1 \
  "$AWG" -f "$AWGCLI" >"$WORKDIR/client.log" 2>&1 &
PIDS+=("$!")
sleep 1
ip -n "$NS_CLI" addr add 10.8.1.2/32 dev "$AWGCLI"
ip -n "$NS_CLI" link set "$AWGCLI" up
ip -n "$NS_CLI" link set "$AWGCLI" mtu 1376
ip -n "$NS_CLI" route add 0.0.0.0/1 dev "$AWGCLI"
ip -n "$NS_CLI" route add 128.0.0.0/1 dev "$AWGCLI"

ROUTER_SETTINGS=""
if [ -z "${NOROUTING:-}" ]; then
  ROUTER_SETTINGS="routing_bypass_ifname=$VCLI
routing_config_file=$ROUTING_CONFIG"
fi
ip netns exec "$NS_CLI" bash -c \
  "printf 'set=1\nprivate_key=%s\n%s\n%s\npublic_key=%s\nendpoint=192.168.100.1:51820\nallowed_ip=0.0.0.0/0\npersistent_keepalive_interval=25\n\n' \
    '$CK' '$OBF' '$ROUTER_SETTINGS' '$SP' | '$H' uapi '$CLIENT_SOCKET'" | grep -q errno=0 || {
      echo "Client configuration failed" >&2
      exit 1
    }
sleep 1
echo "Handshake probe start $(date +%T.%N)"

write_state
if [ -n "${SETUP_ONLY:-}" ]; then
  trap - EXIT INT TERM
  echo "Setup complete"
  exit 0
fi

C=(ip netns exec "$NS_CLI")
resolve() { "${C[@]}" dig +short +time=3 +tries=1 @198.18.0.53 "$@" | tail -1; }
fetch() { "${C[@]}" curl -s --max-time 10 "$@"; }

echo "== DNS through the router"
check 172.31.2.10 "$(resolve proxy.test)" "proxy.test resolved through remote DNS"
check 172.31.1.10 "$(resolve www.direct.test)" "www.direct.test resolved through direct DNS"
check 172.31.4.10 "$(resolve other.test)" "other.test resolved by the default action"
check "" "$(resolve ads.test)" "blocked name has no address answer"
BLOCK_RCODE=$("${C[@]}" dig +time=3 +tries=1 @198.18.0.53 ads.test | sed -n 's/.*status: \([A-Z]*\),.*/\1/p' | head -1)
check NXDOMAIN "$BLOCK_RCODE" "default blocked-name response is NXDOMAIN"

sed 's/"direct": \["172.31.0.53"\]/"direct": ["172.31.0.53"], "blockAnswer": "zero"/' \
  "$ROUTING_CONFIG" > "$ZERO_CONFIG"
printf 'set=1\nrouting_config_file=%s\n\n' "$ZERO_CONFIG" | \
  ip netns exec "$NS_CLI" "$H" uapi "$CLIENT_SOCKET" | grep -q errno=0 || bad "enable zero block response"
check 0.0.0.0 "$(resolve ads.test)" "zero block response returns 0.0.0.0"
printf 'set=1\nrouting_config_file=%s\n\n' "$ROUTING_CONFIG" | \
  ip netns exec "$NS_CLI" "$H" uapi "$CLIENT_SOCKET" | grep -q errno=0 || bad "restore default block response"

check 172.31.2.10 "$("${C[@]}" dig +tcp +short +time=3 +tries=1 @198.18.0.53 proxy.test | tail -1)" "DNS over TCP"
check 172.31.4.10 "$("${C[@]}" dig +short +time=3 +tries=1 @8.8.8.8 other.test | tail -1)" "UDP port 53 interception"

echo "== TCP paths"
check 10.8.1.2 "$(fetch http://172.31.2.10/)" "proxy name uses the tunnel"
check 192.168.100.2 "$(fetch http://172.31.1.10/)" "direct name uses the uplink"
check 192.168.100.2 "$(fetch http://172.31.3.10/)" "direct CIDR uses the uplink"
check 10.8.1.2 "$(fetch http://172.31.4.10/)" "default route uses the tunnel"
check 000 "$("${C[@]}" curl -s -o /dev/null -w '%{http_code}' --max-time 5 http://172.31.5.10/)" "blocked IP rejected"
T0=$(date +%s%N)
"${C[@]}" curl -s -o /dev/null --max-time 5 http://172.31.5.10/ || true
T1=$(date +%s%N)
ELAPSED_MS=$(( (T1 - T0) / 1000000 ))
if [ "$ELAPSED_MS" -lt 1000 ]; then
  ok "blocked IP fails fast ($ELAPSED_MS ms)"
else
  bad "blocked IP took $ELAPSED_MS ms"
fi

echo "== Shared address conflict"
resolve www.direct.test >/dev/null
resolve shared.test >/dev/null
check 10.8.1.2 "$(fetch http://172.31.1.10/)" "proxy rule wins for a shared address"

echo "== UDP paths"
check "from 192.168.100.2 hi" "$("${C[@]}" bash -c 'echo -n hi | nc -u -w2 172.31.3.10 7')" "direct UDP"
check "from 10.8.1.2 hi" "$("${C[@]}" bash -c 'echo -n hi | nc -u -w2 172.31.4.10 7')" "tunneled UDP"

echo "== ICMP"
if "${C[@]}" ping -c1 -W3 172.31.3.10 >/dev/null 2>&1; then
  ok "direct ping"
else
  bad "direct ping"
fi
if "${C[@]}" ping -c1 -W3 172.31.4.10 >/dev/null 2>&1; then
  ok "tunneled ping"
else
  bad "tunneled ping"
fi

echo "== Throughput"
for url in http://172.31.3.10/big http://172.31.4.10/big; do
  RESULT=$(fetch -o /dev/null -w '%{size_download} %{speed_download}' "$url")
  read -r SIZE SPEED <<< "$RESULT"
  if [ "${SIZE:-0}" = 33554432 ]; then
    ok "$url complete, $(( ${SPEED%.*} / 1048576 )) MiB/s"
  else
    bad "$url incomplete: $RESULT"
  fi
done

echo "== Parallel connections"
FLOW_PIDS=()
for index in $(seq 1 50); do
  (result=$(fetch http://172.31.3.10/); [ "$result" = 192.168.100.2 ] || echo x) \
    > "$WORKDIR/parallel.$index.log" &
  FLOW_PIDS+=("$!")
done
for pid in "${FLOW_PIDS[@]}"; do wait "$pid" || true; done
cat "$WORKDIR"/parallel.*.log > "$WORKDIR/parallel.log"
rm -f "$WORKDIR"/parallel.*.log
check 0 "$(grep -c x "$WORKDIR/parallel.log")" "50 parallel direct connections"

echo "== Disable routing at runtime"
if printf 'set=1\nrouting_config_file=\n\n' | \
    ip netns exec "$NS_CLI" "$H" uapi "$CLIENT_SOCKET" | grep -q errno=0; then
  ok "routing disabled"
else
  bad "routing disable failed"
fi
check 10.8.1.2 "$(fetch http://172.31.3.10/)" "disabled router sends traffic through the tunnel"

echo
echo "RESULT: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
