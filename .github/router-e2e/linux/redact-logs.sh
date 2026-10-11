#!/bin/bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
  echo "Usage: $0 <input-directory> <output-directory>" >&2
  exit 2
fi

input=$1
output=$2
mkdir -p "$output"

while IFS= read -r -d '' file; do
  relative=${file#"$input"/}
  destination="$output/$relative"
  mkdir -p "$(dirname "$destination")"
  sed -E \
    -e 's/(private_key|preshared_key|header_protection_key)=([A-Za-z0-9+\/=]+)/\1=[REDACTED_SECRET]/g' \
    -e 's/(Authorization: *(Bearer|token) +)[^[:space:]]+/\1[REDACTED_SECRET]/Ig' \
    -e 's/(gh[pousr]_[A-Za-z0-9_]{20,})/[REDACTED_SECRET]/g' \
    "$file" > "$destination"
done < <(find "$input" -type f -print0)
