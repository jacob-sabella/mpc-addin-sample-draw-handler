#!/usr/bin/env bash
# Host tests (x86): the addin preloaded into a stand-in "MPC Sample" (tests/fake_mpc.c) with a file as the framebuffer,
# under ASan+UBSan: ownership, painting over MPC's refreshes, restore on release, pass-through; the page button
# (a FIFO plays the control surface); then the control
# interface (status, test pattern, grant, release); and that it stays out of a process with another name.
set -euo pipefail
cd "$(dirname "$0")/.."
B=build/host; mkdir -p "$B"
VER="-DADDIN_VERSION=\"$(cat VERSION)\""
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
cc -std=gnu11 -O1 -Wall -Wextra -Werror $SAN -fPIC -shared -fvisibility=hidden $VER -o "$B/sdraw.so" src/sdraw.c src/ctl.c src/input.c -ldl -lpthread
cc -std=gnu11 -O1 -Wall -Wextra $SAN -o "$B/MPC Sample" tests/fake_mpc.c -ldl
cp "$B/MPC Sample" "$B/other"
head -c $((320 * 240 * 4)) /dev/zero > "$B/fb"
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
printf 'enabled=1\nbind=127.0.0.1\nport=%s\npage_note=0\n' "$PORT" > "$B/test.conf"
ASAN="$(cc -print-file-name=libasan.so)"
run() { MPC_SDRAW_CONF="$B/test.conf" MPC_SDRAW_TEST_FB="$B/fb" ASAN_OPTIONS=detect_leaks=0 LD_PRELOAD="$ASAN:$PWD/$B/sdraw.so" "$@"; }
run "$B/MPC Sample"
run "$B/other" none
head -c $((320 * 240 * 4)) /dev/zero > "$B/fb1"; head -c $((320 * 240 * 4)) /dev/zero > "$B/fb2"
MPC_SDRAW_CONF="$B/test.conf" MPC_SDRAW_TEST_FB="$B/fb%u" ASAN_OPTIONS=detect_leaks=0 LD_PRELOAD="$ASAN:$PWD/$B/sdraw.so" "$B/MPC Sample" flip
rm -f "$B/surface.fifo"; mkfifo "$B/surface.fifo"
MPC_SDRAW_TEST_SURFACE="$PWD/$B/surface.fifo" run "$B/MPC Sample" input 2>&1 | grep -v "button: note"
# the control interface, with one client waiting
run "$B/MPC Sample" wait > "$B/wait.log" 2>&1 & P=$!
for _ in $(seq 50); do grep -q ready "$B/wait.log" 2>/dev/null && break; sleep 0.1; done
get() { curl -s -m 5 "http://127.0.0.1:$PORT$1"; }
fail=0; check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
px() { python3 -c "import struct,sys; d=open('$B/fb','rb').read(); x,y=$1,$2; print('%06x' % (struct.unpack_from('<I',d,(y*320+x)*4)[0] & 0xffffff))"; }
check "status lists the waiting client, no owner" '[[ "$(get /status)" == *"\"owner\":0"*"\"name\":\"waiting plugin\""* ]]'
check "test pattern takes the screen" '[ "$(get "/test?on=1")" = ok ] && sleep 0.3 && [ "$(px 5 50)" = ffffff ]'
check "/snap returns the screen as shown (320x240x4, the test pattern)" '[ "$(curl -s -m 5 "http://127.0.0.1:$PORT/snap" | python3 -c "import struct,sys; d=sys.stdin.buffer.read(); print(len(d), \"%06x\" % (struct.unpack_from(\"<I\",d,(50*320+5)*4)[0] & 0xffffff))")" = "307200 ffffff" ]'
ID=$(get /status | python3 -c 'import json,sys; print([c["id"] for c in json.load(sys.stdin)["clients"] if c["name"]=="waiting plugin"][0])')
check "grant gives the waiting client the screen" '[ "$(get "/grant?id=$ID")" = ok ] && [ "$(px 160 120)" = 00aa00 ]'
check "grant of an unknown client is refused" '[[ "$(get "/grant?id=999")" == *"no such client"* ]]'
check "release gives MPC its screen back" '[ "$(get /release)" = ok ] && [ "$(px 160 120)" = 111111 ] && [[ "$(get /status)" == *"\"owner\":0"* ]]'
check "test pattern off" '[ "$(get "/test?on=0")" = ok ] && [[ "$(get /status)" != *"test pattern"* ]]'
kill $P; wait $P 2>/dev/null || true
grep -q "ERROR: AddressSanitizer\|runtime error" "$B/wait.log" && { echo "FAIL sanitizer"; cat "$B/wait.log"; fail=1; }
[ $fail = 0 ] && echo "control tests passed" || { cat "$B/wait.log"; exit 1; }
