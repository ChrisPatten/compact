#!/bin/bash
# Set (or clear) the Compact's secondary Wi-Fi network (a phone hotspot) over the LAN: POST /wifi, Bearer surface token.
# WIFI2_SSID / WIFI2_PASSWORD come from the environment or from .envrc in the repo root; the password is
# sent on stdin and never printed or put on a command line.
#   scripts/wifi2.sh            # set from .envrc
#   scripts/wifi2.sh --clear    # remove the secondary network
#   COMPACT_HOST=192.168.5.x scripts/wifi2.sh
set -euo pipefail
cd "$(dirname "$0")/.."
HOST=${COMPACT_HOST:-compact.local}
TOKEN=$(grep '^INTERCOM_COMPACT_TOKEN=' "$HOME/workspace/agentbus/.env" | cut -d= -f2)
[ -n "$TOKEN" ] || { echo "INTERCOM_COMPACT_TOKEN not found in agentbus/.env" >&2; exit 1; }

envrc_get() {   # value of NAME from .envrc: "export NAME=value" or "NAME=value", optionally quoted
  [ -f .envrc ] || return 0
  local line v
  line=$(grep -E "^[[:space:]]*(export[[:space:]]+)?$1=" .envrc | tail -1) || true
  v=${line#*=}
  v=${v%\"}; v=${v#\"}; v=${v%\'}; v=${v#\'}
  printf '%s' "$v"
}

if [ "${1:-}" = "--clear" ]; then
  body='{"slot":2,"clear":true}'
else
  export WIFI2_SSID=${WIFI2_SSID:-$(envrc_get WIFI2_SSID)}
  export WIFI2_PASSWORD=${WIFI2_PASSWORD:-$(envrc_get WIFI2_PASSWORD)}
  [ -n "$WIFI2_SSID" ] || { echo "WIFI2_SSID not set (environment or .envrc)" >&2; exit 1; }
  body=$(python3 -c 'import json,os; print(json.dumps({"slot":2,"ssid":os.environ["WIFI2_SSID"],"password":os.environ.get("WIFI2_PASSWORD","")}))')
fi
printf '%s' "$body" | curl -4 -sf --max-time 10 -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
     --data-binary @- "http://$HOST/wifi" || { echo "device did not accept it (is it on the LAN at $HOST?)" >&2; exit 1; }
