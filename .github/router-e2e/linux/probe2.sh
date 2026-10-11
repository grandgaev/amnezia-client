#!/bin/bash
# UDP echo through the tunnel with routing disabled
cd "$(dirname "$0")"
NOROUTING=1 SETUP_ONLY=1 timeout 60 ./run.sh > /tmp/e2e_setup.out 2>&1 || { tail -5 /tmp/e2e_setup.out; exit 1; }
sleep ${WAIT:-1}
R=""
for i in 1 2 3 4; do r=$(ip netns exec cli bash -c "echo -n hi$i | nc -u -w1 172.31.4.10 7"); case "$r" in from*) R="$R ok";; *) R="$R FAIL";; esac; done
echo "$R"
pkill -x amneziawg-go; pkill -x e2ehelper; ip netns del cli; ip netns del inet; ip link del hcli 2>/dev/null; ip link del hinet 2>/dev/null
