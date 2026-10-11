#!/bin/bash
# usage: OBF_OVERRIDE="..." ./probe.sh  -> prints results of 3 successive remote DNS queries
cd "$(dirname "$0")"
SETUP_ONLY=1 timeout 60 ./run.sh > /tmp/e2e_setup.out 2>&1 || { tail -5 /tmp/e2e_setup.out; exit 1; }
sleep 1
R=""
for n in proxy.test other.test proxy.test other.test; do r=$(ip netns exec cli dig +short +time=2 +tries=1 @198.18.0.53 $n | tail -1); case "$r" in 172.*) R="$R ok";; *) R="$R FAIL";; esac; done
echo "$R"
pkill -x amneziawg-go; pkill -x e2ehelper; ip netns del cli; ip netns del inet; ip link del hcli 2>/dev/null; ip link del hinet 2>/dev/null
