#!/bin/bash
set -euo pipefail

real_docker=/usr/bin/docker
source_image=amneziavpn/amneziawg-go:latest
digest_file=/run/amnezia-upgrade-e2e-awg-base
manifest_file=/run/amnezia-upgrade-e2e-awg-base.manifest

test -f /.dockerenv
"$real_docker" pull --quiet "$source_image" >/dev/null
base_digest=$("$real_docker" image inspect -f '{{range .RepoDigests}}{{println .}}{{end}}' "$source_image" |
  sed -n '/^amneziavpn\/amneziawg-go@sha256:[0-9a-f]\{64\}$/p' | head -n 1)
test -n "$base_digest"

entrypoint=$("$real_docker" image inspect -f '{{if .Config.Entrypoint}}{{index .Config.Entrypoint 0}}{{end}}' "$base_digest")
entrypoint_json=$("$real_docker" image inspect -f '{{json .Config.Entrypoint}}' "$base_digest")
# shellcheck disable=SC2016
runtime_hashes=$("$real_docker" run --rm \
  --network none \
  --read-only \
  --cap-drop ALL \
  --security-opt no-new-privileges \
  --entrypoint /bin/sh \
  -e "UPGRADE_E2E_BASE_ENTRYPOINT=$entrypoint" \
  "$base_digest" -c '
set -eu
entrypoint=$UPGRADE_E2E_BASE_ENTRYPOINT
awg=$(command -v awg)
awg_quick=$(command -v awg-quick)
awg_go=$(command -v amneziawg-go)
grep -Fq WG_QUICK_USERSPACE_IMPLEMENTATION "$awg_quick"
grep -Fq "type amneziawg" "$awg_quick"
if [ -n "$entrypoint" ]; then
  case "$entrypoint" in
    /*) ;;
    *) entrypoint=$(command -v "$entrypoint") ;;
  esac
  sha256sum "$entrypoint" "$awg" "$awg_quick" "$awg_go"
else
  sha256sum "$awg" "$awg_quick" "$awg_go"
fi
')
hash_count=$(printf '%s\n' "$runtime_hashes" | sed '/^$/d' | wc -l | tr -d ' ')
if [ -n "$entrypoint" ]; then
  test "$hash_count" = 4
else
  test "$hash_count" = 3
fi

printf '%s\n' "$base_digest" > "$digest_file"
{
  printf 'digest=%s\n' "$base_digest"
  printf 'image_id=%s\n' "$("$real_docker" image inspect -f '{{.Id}}' "$base_digest")"
  printf 'entrypoint=%s\n' "${entrypoint:-<none>}"
  printf 'entrypoint_json=%s\n' "$entrypoint_json"
  printf '%s\n' 'awg_quick_userspace_fallback=verified'
  printf '%s\n' "$runtime_hashes"
} > "$manifest_file"
chmod 0600 "$digest_file" "$manifest_file"
chown root:root "$digest_file" "$manifest_file"
