#!/bin/bash
# Push build/intercom_firmware.bin to the Compact over Wi-Fi (POST /ota, Bearer surface token).
# The device reboots into the new image; if that image never pairs with the bus within
# 120 s, the bootloader rolls back to the previous one.
#   scripts/ota.sh            # uses compact.local
#   COMPACT_HOST=192.168.5.x scripts/ota.sh
set -euo pipefail
cd "$(dirname "$0")/.."
HOST=${COMPACT_HOST:-compact.local}
BIN=${BIN:-build/intercom_firmware.bin}   # BIN=build-trace/intercom_firmware.bin for the heap-tracing build
ENV_FILE=${AGENTBUS_ENV:-$HOME/agentbus/.env}   # or export INTERCOM_COMPACT_TOKEN directly
TOKEN=${INTERCOM_COMPACT_TOKEN:-$(grep '^INTERCOM_COMPACT_TOKEN=' "$ENV_FILE" 2>/dev/null | cut -d= -f2)}
[ -f "$BIN" ] || { echo "no $BIN; run idf.py build first" >&2; exit 1; }
[ -n "$TOKEN" ] || { echo "INTERCOM_COMPACT_TOKEN not set; export it or point AGENTBUS_ENV at your AgentBus .env" >&2; exit 1; }
before=$(curl -4 -sf --max-time 5 "http://$HOST/info") || { echo "device not reachable at $HOST" >&2; exit 1; }
echo "before: $before"
curl -4 -sf --max-time 180 -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/octet-stream" \
     --data-binary @"$BIN" "http://$HOST/ota"
for i in $(seq 1 30); do
  sleep 3
  after=$(curl -4 -sf --max-time 3 "http://$HOST/info" 2>/dev/null) || continue
  [ "$after" != "$before" ] && { echo "after:  $after"; exit 0; }
done
echo "device did not come back within 90 s" >&2; exit 1
