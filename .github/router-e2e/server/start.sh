#!/bin/sh
set -eu
cd /lab
mkdir -p /var/run/amneziawg
export GOMAXPROCS=2 LOG_LEVEL=error
./amneziawg-go -f awglab > /lab/awg.log 2>&1 &
awg_pid=$!
http_pid=''
cleanup() {
    kill "$awg_pid" ${http_pid:+"$http_pid"} 2>/dev/null || true
}
trap cleanup EXIT INT TERM
i=0
while [ ! -S /var/run/amneziawg/awglab.sock ]; do
    i=$((i+1))
    [ "$i" -lt 50 ] || exit 1
    sleep .1
done
./e2ehelper uapi /var/run/amneziawg/awglab.sock < /lab/server.uapi | grep -q 'errno=0'
ip addr add 10.77.0.1/24 dev awglab
ip link set awglab mtu 1376 up
iptables -t nat -A POSTROUTING -s 10.77.0.0/24 -o eth0 -j MASQUERADE
iptables -A FORWARD -i awglab -j ACCEPT
iptables -A FORWARD -o awglab -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
./e2ehelper http 0.0.0.0:18080 > /lab/http.log 2>&1 &
http_pid=$!
wait "$awg_pid"
