#!/bin/bash
# End-to-end test of the amneziawg-go router in Linux network namespaces.
#
#   ns cli  (192.168.100.2, awgcli 10.8.1.2)  --veth--  root (192.168.100.1, awgsrv 10.8.1.1)  --veth--  ns inet (172.31.x.x)
#
# inet hosts:  172.31.0.53 DNS, 172.31.1.10 "direct" site, 172.31.2.10 "proxy" site,
#              172.31.3.10 direct by CIDR, 172.31.4.10 default route site, 172.31.5.10 blocked IP
# The HTTP servers answer with the client source address: 192.168.100.2 means the
# connection went directly, 10.8.1.2 means it went through the tunnel.
set -u
D=$(cd "$(dirname "$0")" && pwd)
AWG=$D/amneziawg-go
H=$D/e2ehelper
PASS=0; FAIL=0
ok()   { echo "  PASS: $*"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL: $*"; FAIL=$((FAIL+1)); }
check(){ local want="$1" got="$2" what="$3"; if [ "$got" == "$want" ]; then ok "$what ($got)"; else bad "$what: want '$want' got '$got'"; fi; }

cleanup() {
  pkill -x amneziawg-go 2>/dev/null; pkill -x e2ehelper 2>/dev/null
  ip netns del cli 2>/dev/null; ip netns del inet 2>/dev/null
  ip link del hcli 2>/dev/null; ip link del hinet 2>/dev/null
  rm -f /var/run/amneziawg/awgsrv.sock
}
cleanup
trap cleanup EXIT
sleep 0.5

ip netns add cli; ip netns add inet
ip link add hcli type veth peer name vcli; ip link set vcli netns cli
ip link add hinet type veth peer name inet0; ip link set inet0 netns inet
ip addr add 192.168.100.1/24 dev hcli; ip link set hcli up
ip addr add 172.31.255.1/24 dev hinet; ip link set hinet up
ip route add 172.31.0.0/16 via 172.31.255.2
sysctl -qw net.ipv4.ip_forward=1
ip -n cli addr add 192.168.100.2/24 dev vcli; ip -n cli link set vcli up; ip -n cli link set lo up
ip -n cli route add default via 192.168.100.1
ip -n inet addr add 172.31.255.2/24 dev inet0; ip -n inet link set inet0 up; ip -n inet link set lo up
for a in 172.31.0.53 172.31.1.10 172.31.2.10 172.31.3.10 172.31.4.10 172.31.5.10; do ip -n inet addr add $a/32 dev inet0; done
ip -n inet route add default via 172.31.255.1

# services in the "internet"
ip netns exec inet $H http 172.31.1.10:80 172.31.2.10:80 172.31.3.10:80 172.31.4.10:80 172.31.5.10:80 &
ip netns exec inet $H dns 172.31.0.53:53 "direct.test=172.31.1.10,www.direct.test=172.31.1.10,proxy.test=172.31.2.10,other.test=172.31.4.10,ads.test=172.31.1.10,shared.test=172.31.1.10" 2>/tmp/e2e_dns.log &

read SK SP SB < <($H keygen)
read CK CP CB < <($H keygen)
OBF_BASE="jc=4
jmin=10
jmax=50
s1=12
s2=12
s3=12
s4=12
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
OBF="${OBF_OVERRIDE-$OBF_BASE}"
[ -z "$OBF" ] && OBF="replace_peers=true"

# server
mkdir -p /var/run/amneziawg
LOG_LEVEL=verbose WG_PROCESS_FOREGROUND=1 $AWG -f awgsrv >/tmp/e2e_srv.log 2>&1 &
sleep 1
ip addr add 10.8.1.1/24 dev awgsrv; ip link set awgsrv up
printf "set=1\nprivate_key=$SK\nlisten_port=51820\n$OBF\npublic_key=$CP\nallowed_ip=10.8.1.2/32\n\n" | $H uapi /var/run/amneziawg/awgsrv.sock | grep -q errno=0 || { echo "server config failed"; exit 1; }

# client
cat > /tmp/e2e_routing.json <<EOF
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
ip netns exec cli env LOG_LEVEL=verbose WG_PROCESS_FOREGROUND=1 $AWG -f awgcli >/tmp/e2e_cli.log 2>&1 &
sleep 1
ip -n cli addr add 10.8.1.2/32 dev awgcli; ip -n cli link set awgcli up; ip -n cli link set awgcli mtu 1376
ip -n cli route add 0.0.0.0/1 dev awgcli; ip -n cli route add 128.0.0.0/1 dev awgcli
CLISOCK=/var/run/amneziawg/awgcli.sock
ip netns exec cli bash -c "printf 'set=1\nprivate_key=$CK\n$OBF\n$( [ -n "${NOROUTING:-}" ] || echo 'routing_bypass_ifname=vcli\nrouting_config_file=/tmp/e2e_routing.json\n')public_key=$SP\nendpoint=192.168.100.1:51820\nallowed_ip=0.0.0.0/0\npersistent_keepalive_interval=25\n\n' | $H uapi $CLISOCK" | grep -q errno=0 || { echo "client config failed"; cat /tmp/e2e_cli.log; exit 1; }
sleep 1
echo "handshake-probe start $(date +%T.%N)"

C="ip netns exec cli"
if [ -n "${SETUP_ONLY:-}" ]; then trap - EXIT; echo "setup done"; exit 0; fi
resolve() { $C dig +short +time=3 +tries=1 @198.18.0.53 "$@" | tail -1; }
fetch() { $C curl -s --max-time 10 "$@"; }

echo "== DNS through the router"
check 172.31.2.10 "$(resolve proxy.test)" "proxy.test resolved (remote DNS via tunnel)"
check 172.31.1.10 "$(resolve www.direct.test)" "www.direct.test resolved (direct DNS)"
check 172.31.4.10 "$(resolve other.test)" "other.test resolved (default)"
check "" "$(resolve ads.test)" "ads.test blocked"
check NXDOMAIN "$($C dig +time=3 +tries=1 @198.18.0.53 ads.test | grep -o NXDOMAIN)" "ads.test NXDOMAIN"
check 172.31.2.10 "$($C dig +tcp +short +time=3 +tries=1 @198.18.0.53 proxy.test | tail -1)" "DNS over TCP to router"
check 172.31.4.10 "$($C dig +short +time=3 +tries=1 @8.8.8.8 other.test | tail -1)" "UDP/53 to arbitrary server is intercepted"

echo "== TCP paths"
check 10.8.1.2 "$(fetch http://172.31.2.10/)" "proxy.test via tunnel"
check 192.168.100.2 "$(fetch http://172.31.1.10/)" "direct.test directly (learned from DNS)"
check 192.168.100.2 "$(fetch http://172.31.3.10/)" "172.31.3.0/24 directly (CIDR rule)"
check 10.8.1.2 "$(fetch http://172.31.4.10/)" "default route via tunnel"
check 000 "$($C curl -s -o /dev/null -w '%{http_code}' --max-time 5 http://172.31.5.10/)" "blocked IP rejected"
T0=$(date +%s%N); $C curl -s -o /dev/null --max-time 5 http://172.31.5.10/; T1=$(date +%s%N)
[ $(( (T1-T0)/1000000 )) -lt 1000 ] && ok "blocked IP fails fast ($(( (T1-T0)/1000000 )) ms)" || bad "blocked IP took $(( (T1-T0)/1000000 )) ms"

echo "== Shared address conflict: proxy wins"
resolve www.direct.test >/dev/null
resolve shared.test >/dev/null   # same IP as direct.test but proxy rule
check 10.8.1.2 "$(fetch http://172.31.1.10/)" "address shared by proxy+direct names goes via tunnel"

echo "== UDP paths"
check "from 192.168.100.2 hi" "$($C bash -c 'echo -n hi | nc -u -w2 172.31.3.10 7')" "UDP direct"
check "from 10.8.1.2 hi" "$($C bash -c 'echo -n hi | nc -u -w2 172.31.4.10 7')" "UDP via tunnel"

echo "== ICMP"
$C ping -c1 -W3 172.31.3.10 >/dev/null 2>&1 && ok "ping direct host" || bad "ping direct host"
$C ping -c1 -W3 172.31.4.10 >/dev/null 2>&1 && ok "ping via tunnel" || bad "ping via tunnel"

echo "== Throughput (32 MiB)"
for u in http://172.31.3.10/big http://172.31.4.10/big; do
  R=$(fetch -o /dev/null -w '%{size_download} %{speed_download}' $u)
  set -- $R
  [ "${1:-0}" == 33554432 ] && ok "$u complete, $(( ${2%.*} / 1048576 )) MiB/s" || bad "$u incomplete: $R"
done

echo "== Parallel connections"
FAILS=0
PIDS=""
for i in $(seq 1 50); do (r=$(fetch http://172.31.3.10/); [ "$r" == 192.168.100.2 ] || echo x) > /tmp/e2e_par.$i.log & PIDS="$PIDS $!"; done
wait $PIDS; cat /tmp/e2e_par.*.log > /tmp/e2e_par.log; rm -f /tmp/e2e_par.*.log
check 0 "$(grep -c x /tmp/e2e_par.log)" "50 parallel direct connections"

echo "== Disable routing at runtime"
ip netns exec cli bash -c "printf 'set=1\nrouting_config_file=\n\n' | $H uapi $CLISOCK" | grep -q errno=0 && ok "routing disabled" || bad "disable failed"
check 10.8.1.2 "$(fetch http://172.31.3.10/)" "after disable everything goes through the tunnel"

echo
echo "RESULT: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
