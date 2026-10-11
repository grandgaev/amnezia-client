#!/usr/bin/env python3
import re
import sys

SENSITIVE_LINE = re.compile(
    r"private.?key|preshared.?key|public.?key|header.?protection.?key|"
    r"password|passwd|\bpsk\b|token|secret|authorization|credential|"
    r"(?:vpn|vless|ss|hysteria2)://",
    re.IGNORECASE,
)
KEY_VALUE = re.compile(r"(?<![A-Za-z0-9_])[A-Za-z0-9+/_-]{43,}={0,2}(?![A-Za-z0-9_])")
URL_CREDENTIALS = re.compile(r"(https?://)[^/\s]+@", re.IGNORECASE)


def redact(line):
    if SENSITIVE_LINE.search(line):
        return "[REDACTED]\n"
    line = URL_CREDENTIALS.sub(r"\1[REDACTED]@", line)
    return KEY_VALUE.sub("[REDACTED]", line)


if __name__ == "__main__":
    for line in sys.stdin:
        sys.stdout.write(redact(line))
