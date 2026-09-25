#!/bin/bash
# End-to-end check of the AmneziaWG router "direct" path on macOS, the way the
# desktop daemon drives it: amneziawg-go on a utun, the router enabled over
# UAPI with bypass "interface" (IP_BOUND_IF on the uplink), DNS answered by
# the router at 198.18.0.53, direct names resolved outside the tunnel.
# Only the test destinations are routed into the tunnel: the runner keeps
# its own connectivity. No server is needed: direct traffic never reaches it.
#
# usage: sudo macos-router-e2e.sh <amneziawg-go binary> <unprivileged user>
set -uo pipefail

AWG="$1"
RUN_AS="$2"
WORK=$(mktemp -d)
FAILED=0

log() { echo "=== $*"; }
fail() { echo "FAIL: $*"; FAILED=1; }

uapi() {
    python3 - "$SOCK" "$1" <<'EOF'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
s.sendall(sys.argv[2].encode() + b"\n")
data = b""
while not data.endswith(b"\n\n"):
    chunk = s.recv(65536)
    if not chunk:
        break
    data += chunk
out = data.decode()
print(out.strip())
sys.exit(0 if "errno=0" in out else 1)
EOF
}

cleanup() {
    [ -n "${AWGPID:-}" ] && kill "$AWGPID" 2>/dev/null
    pfctl -a com.apple/amnezia-e2e -F all 2>/dev/null
    sleep 1
    log "amneziawg-go log (router lines)"
    grep -i 'router' "$WORK/awg.log" | tail -60
}
trap cleanup EXIT

UPLINK=$(route -n get default | awk '/interface:/{print $2}')
UPLINK_INDEX=$(python3 -c "import socket,sys; print(socket.if_nametoindex(sys.argv[1]))" "$UPLINK")
UPLINK6_INDEX=0
if route -n get -inet6 default >/dev/null 2>&1; then
    UP6=$(route -n get -inet6 default | awk '/interface:/{print $2}')
    [ -n "$UP6" ] && UPLINK6_INDEX=$(python3 -c "import socket,sys; print(socket.if_nametoindex(sys.argv[1]))" "$UP6")
fi
log "uplink $UPLINK index $UPLINK_INDEX, IPv6 uplink index $UPLINK6_INDEX"

# Resolve the IP-rule test target before anything goes through the tunnel.
IPRULE_HOST=example.com
IPRULE_IP=$(dig +short A "$IPRULE_HOST" | grep -E '^[0-9.]+$' | head -1)
log "$IPRULE_HOST -> $IPRULE_IP"

export WG_TUN_NAME_FILE="$WORK/tun.name" LOG_LEVEL=verbose
"$AWG" -f utun > "$WORK/awg.log" 2>&1 &
AWGPID=$!
for _ in $(seq 50); do [ -s "$WORK/tun.name" ] && break; sleep 0.2; done
TUN=$(cat "$WORK/tun.name")
SOCK="/var/run/amneziawg/$TUN.sock"
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.2; done
log "tunnel $TUN, UAPI $SOCK"

PRIV=$(openssl rand -hex 32)
PEER=$(openssl rand -hex 32)
uapi "set=1
private_key=$PRIV
replace_peers=true
public_key=$PEER
endpoint=192.0.2.1:51820
replace_allowed_ips=true
allowed_ip=0.0.0.0/0
allowed_ip=::/0
" || fail "interface configuration"

ifconfig "$TUN" inet 10.66.0.2 10.66.0.1 netmask 255.255.255.255 up
ifconfig "$TUN" inet6 fd66::2 prefixlen 128 2>/dev/null
ifconfig "$TUN" mtu 1280

cat > "$WORK/routing.json" <<EOF
{"version":1,"defaultAction":"proxy",
 "rules":[{"action":"direct","domains":["domain:ya.ru","domain:ru","domain:example.org"],"ips":["$IPRULE_IP/32"]}],
 "dns":{"listen":["198.18.0.53"],"remote":["1.1.1.1"],"direct":["8.8.8.8"]},
 "bypass":"interface","rejectProxyIpv6":true}
EOF

uapi "set=1
routing_bypass_ifindex=$UPLINK_INDEX
routing_bypass_ifindex6=$UPLINK6_INDEX
routing_config_file=$WORK/routing.json
" || fail "router configuration"

route -n add -host 198.18.0.53 -interface "$TUN" >/dev/null

as_user() { sudo -u "$RUN_AS" "$@"; }

check_https() { # name ip family
    local name="$1" ip="$2" fam="$3" out rc
    if [ "$fam" = 6 ]; then
        route -n add -inet6 -host "$ip" -interface "$TUN" >/dev/null 2>&1
        out=$(as_user curl -6 -sS -o /dev/null -w '%{http_code}' --connect-timeout 10 -m 20 \
            --resolve "$name:443:[$ip]" "https://$name/" 2>&1); rc=$?
    else
        route -n add -host "$ip" -interface "$TUN" >/dev/null 2>&1
        out=$(as_user curl -4 -sS -o /dev/null -w '%{http_code}' --connect-timeout 10 -m 20 \
            --resolve "$name:443:$ip" "https://$name/" 2>&1); rc=$?
    fi
    echo "$name [$ip] -> rc=$rc $out"
    return $rc
}

log "1. DNS through the router (direct name)"
A=$(as_user dig +time=5 +tries=1 @198.18.0.53 ya.ru A +short | grep -E '^[0-9.]+$' | head -1)
AAAA=$(as_user dig +time=5 +tries=1 @198.18.0.53 ya.ru AAAA +short | grep ':' | head -1)
echo "ya.ru A=$A AAAA=$AAAA"
[ -n "$A" ] || fail "no A answer for a direct name"

log "2. Direct TCP to a learned address (IPv4)"
[ -n "$A" ] && { check_https ya.ru "$A" 4 || fail "direct IPv4 connection to ya.ru"; }

log "3. Direct TCP by an IP rule (no DNS through the router)"
[ -n "$IPRULE_IP" ] && { check_https "$IPRULE_HOST" "$IPRULE_IP" 4 || fail "direct IPv4 connection by IP rule"; }

log "4. Direct TCP to a learned address (IPv6), uplink IPv6 index $UPLINK6_INDEX"
if [ -n "$AAAA" ]; then
    if check_https ya.ru "$AAAA" 6; then
        echo "IPv6 direct works"
    elif [ "$UPLINK6_INDEX" = 0 ]; then
        fail "AAAA answered for a direct name although the uplink has no IPv6"
    else
        fail "direct IPv6 connection to ya.ru"
    fi
else
    echo "no AAAA answer (expected without an IPv6 uplink)"
fi

log "5. Direct TCP with the kill switch rule for the router"
# The daemon passes root-owned TCP/UDP (amneziawg-go) with a quick rule; here
# anything else to the test address is refused, as the kill switch would.
if [ -n "$A" ]; then
    pfctl -E 2>/dev/null | grep -F 'Token' > "$WORK/pf.token" || true
    printf 'pass out quick proto { tcp, udp } user root flags any no state\nblock return out quick on %s proto tcp to %s flags any no state\n' "$UPLINK" "$A" \
        | pfctl -q -a com.apple/amnezia-e2e -f - || fail "pf rules"
    check_https ya.ru "$A" 4 || fail "direct IPv4 connection with the kill switch rule"
    pfctl -a com.apple/amnezia-e2e -F all 2>/dev/null
fi

log "6. Parallel direct connections"
if [ -n "$A" ]; then
    pids=()
    for i in $(seq 20); do
        ( as_user curl -4 -s -o /dev/null --connect-timeout 10 -m 20 --resolve "ya.ru:443:$A" https://ya.ru/ \
            && touch "$WORK/ok.$i" ) &
        pids+=($!)
    done
    # Only these jobs: amneziawg-go runs in the background too.
    wait "${pids[@]}"
    ok=$(ls "$WORK"/ok.* 2>/dev/null | wc -l | tr -d ' ')
    echo "$ok/20 succeeded"
    [ "$ok" = 20 ] || fail "parallel direct connections: $ok/20"
fi

if [ "$FAILED" != 0 ]; then
    echo "SOME CHECKS FAILED"
    exit 1
fi
echo "ALL CHECKS PASSED"
