#!/usr/bin/env bash
# vctools smoke test.
#
# The point of vctools is that a test can drive a real GUI without touching the
# live session, so the suite checks both halves: that input and capture actually
# work inside the box, and that the host's windows are byte-for-byte where they
# were when it started.
set -u

VC="$(cd "$(dirname "$0")/.." && pwd)/vc"
BOX="vctest$$"
TMP="$(mktemp -d)"
PASS=0
FAIL=0

ok()   { printf '  \033[32mok\033[0m   %s\n' "$1"; PASS=$((PASS + 1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAIL=$((FAIL + 1)); }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (want '$3', got '$2')"; fi; }

cleanup() {
	"$VC" box kill "$BOX" >/dev/null 2>&1
	"$VC" box kill "${BOX}s" >/dev/null 2>&1
	rm -rf "$TMP"
}
trap cleanup EXIT

command -v atriumctl >/dev/null && atriumctl version >/dev/null 2>&1 || { echo "these tests need an atrium host"; exit 77; }
[ -x "$VC" ] || { echo "build vc first (make)"; exit 1; }

# A window's app, space and size are exactly what a box leaking into the live
# session would disturb, and so are the session's own: the environment systemd
# hands new apps, and who answers notifications. The helper lives in a file
# because quoting a JSON-munging script through the shell twice is how you end
# up with a comparison that silently compares nothing.
cat > "$TMP/snap.py" <<'PY'
import json, sys
rows = []
for w in json.load(sys.stdin):
    g = w["geometry"]
    rows.append("%s|%s|%s|%dx%d" % (w["id"], w["app_id"], w["space"], g["width"], g["height"]))
print("\n".join(sorted(rows)))
PY

snapshot() {
	atriumctl -j windows | python3 "$TMP/snap.py"
	systemctl --user show-environment | grep -E '^(WAYLAND_DISPLAY|DISPLAY)='
	busctl --user call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus \
		GetConnectionUnixProcessID s org.freedesktop.Notifications 2>/dev/null
}

# atriumctl against a box.
boxctl() {
	local b=$1; shift
	(eval "$("$VC" box env "$b")"; XDG_RUNTIME_DIR="$(dirname "$WAYLAND_DISPLAY")" atriumctl -s "$ATRIUM_SOCKET" "$@")
}

echo "vctools smoke test (box: $BOX)"
snapshot > "$TMP/host-before"

# --- box lifecycle ---------------------------------------------------------

if "$VC" box start "$BOX" --size 800x600 >"$TMP/start.log" 2>&1; then
	ok "box start"
else
	bad "box start"; cat "$TMP/start.log"; exit 1
fi

check "box reports its size" "$(grep -o '[0-9]*x[0-9]*' "$TMP/start.log" | tail -1)" "800x600"
check "box is listed as running" "$("$VC" box list | awk -v b="$BOX" '$1==b{print $4}')" "running"

# --- input -----------------------------------------------------------------

# `cat` in a terminal is a keystroke oracle: whatever the app really received
# ends up in the file, so this cannot pass by accident.
"$VC" box exec "$BOX" -- foot -T sink sh -c "cat > $TMP/typed.txt" >/dev/null 2>&1 &
if "$VC" wait "$BOX" window foot 10 >/dev/null 2>&1; then
	ok "wait for window"
else
	bad "wait for window"
fi

"$VC" in "$BOX" type "vctools ok" key Return >/dev/null 2>&1
sleep 0.4
check "typed text arrives intact" "$(cat "$TMP/typed.txt" 2>/dev/null)" "vctools ok"

# A click focuses the window under it, so clicking the middle of the sink
# while another window is in front shows the pointer landed where asked.
"$VC" box exec "$BOX" -- foot -T other sleep 600 >/dev/null 2>&1 &
"$VC" wait "$BOX" window other 10 >/dev/null 2>&1
sleep 0.5
# New windows open in the middle, over the sink's; this one goes aside.
OTHER=$(boxctl "$BOX" -j windows | python3 -c 'import json,sys; print(next(w["id"] for w in json.load(sys.stdin) if w["title"] == "other"))')
boxctl "$BOX" move "$OTHER" 0 400 >/dev/null
sleep 0.3
read -r SINK CX CY <<<"$(boxctl "$BOX" -j windows | python3 -c '
import json, sys
for w in json.load(sys.stdin):
    if w["title"] == "sink":
        g = w["geometry"]; print(w["id"], g["x"] + g["width"] // 2, g["y"] + g["height"] // 2)')"
"$VC" in "$BOX" move "$CX" "$CY" click >/dev/null 2>&1
sleep 0.3
check "pointer lands where asked" \
	"$(boxctl "$BOX" -j windows | python3 -c 'import json,sys; print(next((w["id"] for w in json.load(sys.stdin) if w["focused"]), ""))')" "$SINK"

# A wheel event reaches nobody until the virtual pointer has actually moved, so
# this has to be the FIRST pointer op in a box of its own: run it after anything
# else has moved the pointer and it passes whether or not that still works.
SCROLLBOX="${BOX}s"
if "$VC" box start "$SCROLLBOX" --size 800x600 >/dev/null 2>&1; then
	seq 1 400 | awk '{printf "line %04d\n", $1}' > "$TMP/lines.txt"
	"$VC" box exec "$SCROLLBOX" -- foot -T pager less "$TMP/lines.txt" >/dev/null 2>&1 &
	"$VC" wait "$SCROLLBOX" window foot 10 >/dev/null 2>&1
	sleep 1
	"$VC" shot "$SCROLLBOX" pager "$TMP/pager-before.png" >/dev/null 2>&1
	"$VC" in "$SCROLLBOX" scroll 5 >/dev/null 2>&1
	sleep 0.6
	"$VC" shot "$SCROLLBOX" pager "$TMP/pager-after.png" >/dev/null 2>&1
	if cmp -s "$TMP/pager-before.png" "$TMP/pager-after.png"; then
		bad "wheel scrolls a client with no prior move"
	else
		ok "wheel scrolls a client with no prior move"
	fi
	"$VC" box kill "$SCROLLBOX" >/dev/null 2>&1
else
	bad "scroll box start"
fi

# --- capture ---------------------------------------------------------------

check "screen capture size" "$("$VC" shot "$BOX" "$TMP/screen.png" | awk '{print $2}')" "800x600"
if [ -s "$TMP/screen.png" ] && [ "$(head -c4 "$TMP/screen.png" | tail -c3)" = "PNG" ]; then
	ok "capture is a real PNG"
else
	bad "capture is a real PNG"
fi
if "$VC" shot "$BOX" foot "$TMP/window.png" >/dev/null 2>&1 && [ -s "$TMP/window.png" ]; then
	ok "window capture"
else
	bad "window capture"
fi

# --- scripting -------------------------------------------------------------

cat > "$TMP/script.vc" <<EOF
# comments and blank lines are skipped

type "from a script"
key Return
sleep 0.3
shot $TMP/script.png
EOF
"$VC" run "$BOX" "$TMP/script.vc" >/dev/null 2>&1
sleep 0.3
if grep -q "from a script" "$TMP/typed.txt" 2>/dev/null; then
	ok "script runs ops in order"
else
	bad "script runs ops in order"
fi

# --- the invariant that matters --------------------------------------------

snapshot > "$TMP/host-after"
if diff -q "$TMP/host-before" "$TMP/host-after" >/dev/null; then
	ok "host windows and session untouched"
else
	bad "host windows or session moved:"
	diff "$TMP/host-before" "$TMP/host-after" | sed 's/^/       /'
fi

SOCK=$(eval "$("$VC" box env "$BOX")"; echo "$WAYLAND_DISPLAY")
"$VC" box kill "$BOX" >/dev/null 2>&1
sleep 0.5
if [ -S "$SOCK" ]; then
	bad "box kill removes the socket"
else
	ok "box kill removes the socket"
fi

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
