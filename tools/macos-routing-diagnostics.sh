#!/bin/bash
# Collects everything needed to debug routing profiles (the AmneziaWG router)
# on a Mac while the VPN is CONNECTED. Run with: sudo bash macos-routing-diagnostics.sh [AppName] [direct-site] [proxied-site]
# Writes a redacted report to ~/Desktop/amnezia-routing-diag-<time>.txt.
umask 077
APP="${1:-AmneziaVPNTest}"
DIRECT="${2:-ya.ru}"
PROXY="${3:-youtube.com}"
LC_APP=$(echo "$APP" | tr '[:upper:]' '[:lower:]')
SUFFIX=""; [ "$APP" != "AmneziaVPN" ] && SUFFIX="-${LC_APP#amneziavpn}"
PF_ANCHOR="amn$SUFFIX"
REAL_USER="${SUDO_USER:-$USER}"
OUT="/Users/$REAL_USER/Desktop/amnezia-routing-diag-$(date +%Y%m%d-%H%M%S).txt"
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)

run() { echo; echo "### \$ $*"; "$@" 2>&1; }
as_user() { sudo -u "$REAL_USER" "$@"; }

{
echo "App: $APP  direct: $DIRECT  proxied: $PROXY  suffix: '$SUFFIX'"
run sw_vers
run date -u
run uname -a
echo; echo "### processes"
ps -axo pid,user,group,comm | grep -Ei 'amnezia|wireguard' | grep -v grep
run ls -la /var/run/amneziawg /var/run/amneziawg/routing* "/var/run/amneziavpn$SUFFIX"
for f in /var/run/amneziawg/routing$SUFFIX/*.json; do
    [ -f "$f" ] && { echo; echo "### $f (rules shortened)"; python3 -c "
import json,sys
d=json.load(open(sys.argv[1]))
for r in d.get('rules',[]):
    r['domains']=r.get('domains',[])[:5]+(['...%d total'%len(r.get('domains',[]))] if len(r.get('domains',[]))>5 else [])
    r['ips']=r.get('ips',[])[:5]+(['...%d total'%len(r.get('ips',[]))] if len(r.get('ips',[]))>5 else [])
print(json.dumps(d,indent=1,ensure_ascii=False))" "$f"; }
done
for s in /var/run/amneziawg/utun*.sock; do
    [ -S "$s" ] || continue
    echo; echo "### UAPI get $s (keys removed)"
    nice -n 15 python3 - "$s" <<'PY'
import socket, sys
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
    s.settimeout(3)
    s.connect(sys.argv[1])
    s.sendall(b'get=1\n\n')
    data = b''
    while not data.endswith(b'\n\n') and len(data) < 1024 * 1024:
        part = s.recv(65536)
        if not part:
            break
        data += part
    for line in data.decode(errors='replace').splitlines():
        if 'key' not in line.lower():
            print(line)
PY
done
run ifconfig
run netstat -rn -f inet
run netstat -rn -f inet6
run route -n get default
run route -n get -inet6 default
run scutil --dns
run scutil --nwi
echo; echo "### DNS through the router"
for t in A AAAA; do
    run as_user dig +time=3 +tries=1 @198.18.0.53 "$DIRECT" $t
    run as_user dig +time=3 +tries=1 @198.18.0.53 "$PROXY" $t
done
run as_user dig +time=3 +tries=1 "$DIRECT"
echo; echo "### connections"
run as_user curl -sS -o /dev/null -v --connect-timeout 10 -m 20 "https://$DIRECT/"
run as_user curl -4 -sS -o /dev/null -v --connect-timeout 10 -m 20 "https://$DIRECT/"
run as_user curl -6 -sS -o /dev/null -v --connect-timeout 10 -m 20 "https://$DIRECT/"
run as_user curl -sS -o /dev/null -w 'proxied site: %{http_code} via %{remote_ip}\n' --connect-timeout 10 -m 20 "https://$PROXY/"
run as_user curl -sS -m 10 https://ifconfig.co/ip
echo; echo "### pf"
run pfctl -s info
run pfctl -s Anchors
run pfctl -a "com.apple" -s Anchors
run pfctl -a "$PF_ANCHOR" -s rules
run pfctl -a "$PF_ANCHOR/*" -v -s rules
run pfctl -a "$PF_ANCHOR/*" -s Tables
run pfctl -s rules
echo; echo "### third-party network filters (Little Snitch, LuLu, ...)"
run systemextensionsctl list
ls /Library/Little\ Snitch* /Applications/LuLu.app 2>/dev/null
echo; echo "### logs"
for d in "/var/log/$APP" "/Library/Logs/$APP" "/Users/$REAL_USER/Library/Application Support/$APP.ORG" "/Users/$REAL_USER/Library/Logs/$APP"; do
    [ -e "$d" ] && find "$d" -type f -name '*.log' -mtime -1 2>/dev/null | while read -r f; do
        echo; echo "### tail $f"
        tail -400 "$f"
    done
done
} 2>&1 | nice -n 15 python3 "$SCRIPT_DIR/macos-routing-diagnostics-sanitize.py" > "$OUT"
chown "$REAL_USER" "$OUT"
echo "Saved: $OUT"
