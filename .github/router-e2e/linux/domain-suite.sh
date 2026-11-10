DIRECT_SRC=192.168.100.2
TUNNEL_SRC=10.8.1.2

declare -A NAME_ADDRESS=(
  [example.test]=172.31.10.1
  [www.example.test]=172.31.10.2
  [a.b.example.test]=172.31.10.3
  [a.b.c.d.example.test]=172.31.10.4
  [badexample.test]=172.31.10.5
  [example.test.other.test]=172.31.10.6
  [xexample.test]=172.31.10.7
  [example.testx]=172.31.10.8
  [other.test]=172.31.10.9
  [test]=172.31.10.10
  [example-test.test]=172.31.10.11
)
MATCHING=(example.test www.example.test a.b.example.test a.b.c.d.example.test)
NEAR_MISSES=(badexample.test example.test.other.test xexample.test example.testx other.test test example-test.test)

declare -A SERVED_BY
for name in "${!NAME_ADDRESS[@]}"; do SERVED_BY[$name]=${NAME_ADDRESS[$name]}; done

mkdir -p "/etc/netns/$NS_CLI"
echo "nameserver 198.18.0.53" > "/etc/netns/$NS_CLI/resolv.conf"
trap 'rm -rf "/etc/netns/$NS_CLI"; cleanup' EXIT

write_dns_table() {
  : > "$DNS_TABLE_FILE"
  local name
  for name in "${!SERVED_BY[@]}"; do echo "$name=${SERVED_BY[$name]}" >> "$DNS_TABLE_FILE"; done
  kill -HUP "$DNS_PID"
  sleep 0.3
}

apply_profile() {
  local default_action=$1 rules=$2 extra_dns=${3:-}
  cat > "$ROUTING_CONFIG" <<PROFILE
{
  "version": 1,
  "defaultAction": "$default_action",
  "rules": [$rules],
  "dns": {"listen": ["198.18.0.53"], "remote": ["172.31.0.53"], "direct": ["172.31.0.53"]$extra_dns},
  "bypass": "interface"
}
PROFILE
  local reply
  reply=$(printf 'set=1\nrouting_config_file=%s\n\n' "$ROUTING_CONFIG" | ip netns exec "$NS_CLI" "$H" uapi "$CLIENT_SOCKET")
  [[ $reply == *errno=0* ]] || bad "profile reload failed: $reply"
}

rule() { printf '{"action": "%s", "domains": [%s]}' "$1" "$2"; }

by_name() { "${C[@]}" curl -s --max-time 6 "http://$1/who"; }

expect_path() {
  local name=$1 want=$2 what=$3 got
  got=$(by_name "$name")
  case $want in
    direct) check "$DIRECT_SRC ${SERVED_BY[$name]}" "$got" "$what: $name went direct" ;;
    tunnel) check "$TUNNEL_SRC ${SERVED_BY[$name]}" "$got" "$what: $name went through the tunnel" ;;
    blocked) check "" "$got" "$what: $name was blocked" ;;
  esac
}

expect_dns_rcode() {
  local name=$1 want=$2 what=$3 got
  got=$("${C[@]}" dig +time=3 +tries=1 @198.18.0.53 "$name" | sed -n 's/.*status: \([A-Z]*\),.*/\1/p' | head -1)
  check "$want" "$got" "$what: $name DNS status"
}

run_action_case() {
  local action=$1 default_action=$2 matched_path other_path
  local title="domain:example.test -> $action, default $default_action"
  echo "== $title"
  apply_profile "$default_action" "$(rule "$action" '"domain:example.test"')"
  case $action in
    direct) matched_path=direct ;;
    proxy) matched_path=tunnel ;;
    block) matched_path=blocked ;;
  esac
  case $default_action in
    direct) other_path=direct ;;
    proxy) other_path=tunnel ;;
  esac
  local name
  for name in "${MATCHING[@]}"; do
    expect_path "$name" "$matched_path" "$title"
    [ "$action" = block ] && expect_dns_rcode "$name" NXDOMAIN "$title"
  done
  for name in "${NEAR_MISSES[@]}"; do
    expect_path "$name" "$other_path" "$title"
    expect_dns_rcode "$name" NOERROR "$title"
  done
}

write_dns_table
for case_spec in "direct proxy" "proxy direct" "block proxy" "block direct"; do
  run_action_case $case_spec
done

echo "== Name case and trailing dot"
apply_profile proxy "$(rule direct '"domain:example.test"')"
check "$DIRECT_SRC ${SERVED_BY[www.example.test]}" "$(by_name WWW.Example.TEST)" "upper-case name matches"
check "$DIRECT_SRC ${SERVED_BY[a.b.example.test]}" "$(by_name a.b.example.test.)" "name with trailing dot matches"
apply_profile proxy "$(rule direct '"domain:EXAMPLE.test."')"
check "$DIRECT_SRC ${SERVED_BY[a.b.example.test]}" "$(by_name a.b.example.test)" "rule written with case and trailing dot matches"
apply_profile proxy "$(rule direct '"domain:.example.test"')"
check "$DIRECT_SRC ${SERVED_BY[a.b.example.test]}" "$(by_name a.b.example.test)" "rule written with a leading dot matches"

echo "== full: and keyword: do not widen domain:"
apply_profile proxy "$(rule direct '"full:example.test"')"
expect_path example.test direct "full:example.test"
expect_path www.example.test tunnel "full:example.test"
apply_profile proxy "$(rule direct '"keyword:example.test"')"
expect_path badexample.test direct "keyword:example.test"

echo "== DNS transport and resolver"
apply_profile proxy "$(rule direct '"domain:example.test"')"
check "${SERVED_BY[a.b.example.test]}" "$("${C[@]}" dig +tcp +short +time=3 +tries=1 @198.18.0.53 a.b.example.test | tail -1)" "TCP DNS answer"
"${C[@]}" dig +short +time=3 +tries=1 @8.8.8.8 a.b.example.test >/dev/null
check "$DIRECT_SRC ${SERVED_BY[a.b.example.test]}" "$("${C[@]}" curl -s --max-time 6 "http://${SERVED_BY[a.b.example.test]}/who")" "name learned from a query to another resolver is direct"
"${C[@]}" dig +tcp +short +time=3 +tries=1 @198.18.0.53 badexample.test >/dev/null
check "$TUNNEL_SRC ${SERVED_BY[badexample.test]}" "$("${C[@]}" curl -s --max-time 6 "http://${SERVED_BY[badexample.test]}/who")" "near miss learned over TCP DNS stays tunneled"
check "" "$("${C[@]}" dig +short +time=3 +tries=1 @198.18.0.53 AAAA a.b.example.test | tail -1)" "AAAA query is answered"

echo "== Learned address and a connection by address"
apply_profile proxy "$(rule direct '"domain:example.test"')"
check "$TUNNEL_SRC ${SERVED_BY[example.test]}" "$("${C[@]}" curl -s --max-time 6 "http://${SERVED_BY[example.test]}/who")" "bare address before any lookup follows the default"
resolved=$("${C[@]}" dig +short +time=3 +tries=1 @198.18.0.53 example.test | tail -1)
check "${SERVED_BY[example.test]}" "$resolved" "lookup returns the site address"
check "$DIRECT_SRC ${SERVED_BY[example.test]}" "$("${C[@]}" curl -s --max-time 6 "http://${SERVED_BY[example.test]}/who")" "bare address after the lookup follows the domain rule"

echo "== Profile switch drops learned addresses"
apply_profile proxy "$(rule block '"domain:example.test"')"
check "$TUNNEL_SRC ${SERVED_BY[example.test]}" "$("${C[@]}" curl -s --max-time 6 "http://${SERVED_BY[example.test]}/who")" "address learned under direct is not kept after switching to block"
apply_profile proxy "$(rule direct '"domain:example.test"')"
expect_path example.test direct "switch back to direct"
apply_profile direct "$(rule proxy '"domain:example.test"')"
expect_path example.test tunnel "switch to proxy with default direct"
expect_path other.test direct "switch to proxy with default direct"
apply_profile proxy ""
expect_path example.test tunnel "profile without rules"
expect_path other.test tunnel "profile without rules"

echo "== Changed answer for a name"
SERVED_BY[example.test]=172.31.10.12
apply_profile proxy "$(rule direct '"domain:example.test"')"
write_dns_table
expect_path example.test direct "re-pointed example.test"
check "$TUNNEL_SRC 172.31.10.1" "$("${C[@]}" curl -s --max-time 6 http://172.31.10.1/who)" "old address never looked up after re-pointing follows the default"
SERVED_BY[example.test]=172.31.10.1
write_dns_table

echo "== Shared address"
SERVED_BY[example.test]=172.31.10.13
SERVED_BY[badexample.test]=172.31.10.13
write_dns_table
lookup() { "${C[@]}" dig +short +time=3 +tries=1 @198.18.0.53 "$1" >/dev/null; }
from_address() { "${C[@]}" curl -s --max-time 6 "http://172.31.10.13/who"; }
apply_profile proxy "$(rule direct '"domain:example.test"')"
lookup badexample.test
lookup example.test
check "$DIRECT_SRC 172.31.10.13" "$(from_address)" "direct name looked up last: the shared address goes direct"
apply_profile proxy "$(rule direct '"domain:example.test"')"
lookup example.test
lookup badexample.test
check "$TUNNEL_SRC 172.31.10.13" "$(from_address)" "unmatched name looked up last: the shared address goes through the tunnel"
apply_profile direct "$(rule proxy '"domain:example.test"')"
lookup badexample.test
lookup example.test
check "$TUNNEL_SRC 172.31.10.13" "$(from_address)" "tunnel name looked up last: the shared address goes through the tunnel"
apply_profile direct "$(rule proxy '"domain:example.test"')"
lookup example.test
lookup badexample.test
check "$TUNNEL_SRC 172.31.10.13" "$(from_address)" "unmatched name looked up last: the tunnel rule still wins"
SERVED_BY[example.test]=172.31.10.1
SERVED_BY[badexample.test]=172.31.10.5
write_dns_table

echo "== Rule priority"
apply_profile proxy "$(rule block '"domain:a.b.example.test"'), $(rule direct '"domain:example.test"')"
expect_path a.b.example.test blocked "specific block before wide direct"
expect_path www.example.test direct "specific block before wide direct"
expect_path example.test direct "specific block before wide direct"
apply_profile proxy "$(rule direct '"domain:example.test"'), $(rule block '"domain:a.b.example.test"')"
expect_path a.b.example.test direct "wide direct before specific block"
apply_profile direct "$(rule proxy '"domain:b.example.test"'), $(rule block '"domain:example.test"')"
expect_path a.b.example.test tunnel "subdomain proxy before wide block"
expect_path www.example.test blocked "subdomain proxy before wide block"
expect_path example.test blocked "subdomain proxy before wide block"
apply_profile proxy "$(rule direct '"domain:example.test"'), $(rule proxy '"domain:example.test"')"
expect_path www.example.test direct "same domain twice, first rule wins"

echo "== Block answer mode"
apply_profile proxy "$(rule block '"domain:example.test"')" ', "blockAnswer": "zero"'
check 0.0.0.0 "$("${C[@]}" dig +short +time=3 +tries=1 @198.18.0.53 a.b.example.test | tail -1)" "zero answer for a deep subdomain"
check 0.0.0.0 "$("${C[@]}" dig +short +time=3 +tries=1 @198.18.0.53 example.test | tail -1)" "zero answer for the domain"
check "${SERVED_BY[badexample.test]}" "$("${C[@]}" dig +short +time=3 +tries=1 @198.18.0.53 badexample.test | tail -1)" "near miss keeps its real address in zero mode"
check "" "$(by_name a.b.example.test)" "connection by name to a zeroed answer fails"

echo "== Established connection during a profile switch"
stream_across_switch() {
  local name=$1 expected_source=$2 next_default=$3 next_rules=$4 what=$5 first lines
  apply_profile proxy "$(rule direct '"domain:example.test"')"
  lookup "$name"
  "${C[@]}" curl -sS --max-time 30 "http://${SERVED_BY[$name]}/stream?seconds=8" > "$WORKDIR/switch-stream.log" 2>"$WORKDIR/switch-stream.err" &
  local stream_pid=$!
  PIDS+=("$stream_pid")
  sleep 3
  apply_profile "$next_default" "$next_rules"
  wait "$stream_pid" || true
  lines=$(wc -l < "$WORKDIR/switch-stream.log")
  first=$(head -1 "$WORKDIR/switch-stream.log" | cut -d' ' -f1)
  check "$expected_source" "$first" "$what: stream started on the expected path"
  if awk -v host="$first" '$1 != host { bad=1 } END { exit bad }' "$WORKDIR/switch-stream.log"; then
    ok "$what: stream never changed its source address ($lines of 9 lines delivered)"
  else
    bad "$what: stream changed its source address"
  fi
  echo "  INFO: $what, $lines of 9 lines delivered"
}
stream_across_switch example.test "$DIRECT_SRC" proxy "$(rule direct '"domain:example.test"')" "direct stream, same profile reloaded"
stream_across_switch example.test "$DIRECT_SRC" proxy "$(rule block '"domain:example.test"')" "direct stream, switched to block"
stream_across_switch example.test "$DIRECT_SRC" direct "$(rule proxy '"domain:example.test"')" "direct stream, switched to tunnel"
stream_across_switch other.test "$TUNNEL_SRC" proxy "$(rule block '"domain:example.test"')" "tunnel stream, unrelated rule changed"
