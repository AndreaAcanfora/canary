#!/usr/bin/env bash
# End-to-end test of the Linux port against the OHP reference server:
# canary must report each probe, and report changes as soon as they happen.
# Needs: build/canary, ohp (go install github.com/AndreaAcanfora/openheartbeat/cmd/ohp@latest),
# curl and python3.
set -euo pipefail

cd "$(dirname "$0")/.."
OHP=${OHP:-ohp}
PORT=${PORT:-18080}
KEY=e2etestkey0123456789abcdef012345
work=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$work"' EXIT

"$OHP" serve -addr "127.0.0.1:$PORT" -key "$KEY" 2>"$work/ohp.log" &
for _ in $(seq 50); do
  curl -fs "http://127.0.0.1:$PORT/.well-known/ohp" >/dev/null && break
  sleep 0.1
done

base="http://127.0.0.1:$PORT/ping/$KEY"
echo -18000 >"$work/freezer"   # millidegrees, like /sys/bus/w1/devices/28-*/temperature
echo 0 >"$work/leak"
cat >"$work/canary.conf" <<EOF
name=e2e
probe1.type=alive
probe1.url=$base/alive?create=1
probe2.type=temp
probe2.url=$base/freezer?create=1
probe2.source=$work/freezer
probe2.scale=0.001
probe2.max=-15
probe3.type=contact
probe3.url=$base/leak?create=1
probe3.source=$work/leak
probe3.active=high
EOF

./build/canary -n -c "$work/canary.conf"
./build/canary -v -c "$work/canary.conf" 2>"$work/canary.log" &

checks() { curl -fs -H "Authorization: Bearer $KEY" "http://127.0.0.1:$PORT/api/checks"; }

# expect SLUG SIGNAL BODY_SUBSTRING: waits until the last event of the check matches.
expect() {
  for _ in $(seq 60); do
    if checks | python3 -c '
import json, sys
slug, signal, body = sys.argv[1:]
for c in json.load(sys.stdin):
    if c["slug"] == slug and c["events"]:
        e = c["events"][-1]
        sys.exit(0 if e["signal"] == signal and body in e.get("body", "") and e.get("host") else 1)
sys.exit(1)' "$@"; then
      echo "ok: $*"
      return 0
    fi
    sleep 0.2
  done
  echo "FAIL: expected $*" >&2
  checks >&2 || true
  cat "$work/canary.log" >&2
  exit 1
}

expect alive success "alive uptime_s="
expect freezer success "temp_c=-18.00"
expect leak success "contact=idle"

echo -12500 >"$work/freezer"
expect freezer fail 'temp_c=-12.50 problem="above max"'
echo 1 >"$work/leak"
expect leak fail 'contact=active'

echo -17000 >"$work/freezer"
expect freezer success "temp_c=-17.00"
echo 0 >"$work/leak"
expect leak success "contact=idle"
rm "$work/leak"
expect leak fail 'problem="sensor error"'

echo "e2e: all checks passed"
