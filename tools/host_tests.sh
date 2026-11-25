#!/bin/sh
# Every host test, built with ASan/UBSan and run. Usage: tools/host_tests.sh [outdir]   (default /tmp/compact-host-tests)
# Exits non-zero on the first failure. cJSON comes from ESP-IDF (IDF_PATH, else ~/esp/esp-idf).
set -e
cd "$(dirname "$0")/.."
OUT=${1:-/tmp/compact-host-tests}
IDF=${IDF_PATH:-$HOME/esp/esp-idf}
CJSON=$IDF/components/json/cJSON
mkdir -p "$OUT"
CC=${CC:-cc}
FLAGS="-O1 -g -std=c11 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Imain"

run() {
    name=$1; shift
    # shellcheck disable=SC2086
    $CC $FLAGS "$@" -lm -o "$OUT/$name"
    "$OUT/$name" > "$OUT/$name.log" 2>&1 || { cat "$OUT/$name.log"; echo "FAIL: $name"; exit 1; }
    echo "ok: $name ($(tail -1 "$OUT/$name.log"))"
}

run rim12_test     test/host/rim12_test.c main/rim12.c
run mom_logic_test test/host/mom_logic_test.c main/mom_logic.c main/rim12.c
run ring_px_test   test/host/ring_px_test.c main/ring_px.c main/panel_img.c
run wd_logic_test  test/host/wd_logic_test.c
run touch_logic_test test/host/touch_logic_test.c main/touch_logic.c
run pomo_logic_test test/host/pomo_logic_test.c main/pomo_logic.c
run cmd_logic_test test/host/cmd_logic_test.c main/cmd_logic.c
run gap_text_test test/host/gap_text_test.c main/gap_text.c
run reply_logic_test -Wno-deprecated-declarations -I"$CJSON" test/host/reply_logic_test.c main/reply_logic.c "$CJSON/cJSON.c"
echo "host tests: all passed"
