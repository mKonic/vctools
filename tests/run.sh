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
	rm -rf "$TMP"
}
trap cleanup EXIT

command -v hyprctl >/dev/null || { echo "these tests need a Hyprland host"; exit 77; }
[ -x "$VC" ] || { echo "build vc first (make)"; exit 1; }

# A window's class, workspace and size are exactly what a badly behaved nested
# compositor would disturb, so snapshot all three. The helper lives in a file
# because quoting a JSON-munging script through the shell twice is how you end
# up with a comparison that silently compares nothing.
cat > "$TMP/snap.py" <<'PY'
import json, sys
rows = []
for w in json.load(sys.stdin):
    if w["class"] == "aquamarine":  # vc box windows are expected to come and go
        continue
    rows.append("%s|%s|%s" % (w["class"], w["workspace"]["name"], w["size"]))
print("\n".join(sorted(rows)))
PY

snapshot() {
	hyprctl clients -j | python3 "$TMP/snap.py"
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

SIG=$(grep '^signature=' "${XDG_RUNTIME_DIR:-/tmp}/vctools/$BOX/box" | cut -d= -f2)
"$VC" in "$BOX" move 320 240 >/dev/null 2>&1
check "pointer lands where asked" \
	"$(HYPRLAND_INSTANCE_SIGNATURE=$SIG hyprctl cursorpos 2>/dev/null)" "320, 240"

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
	ok "host windows untouched"
else
	bad "host windows moved or resized:"
	diff "$TMP/host-before" "$TMP/host-after" | sed 's/^/       /'
fi

"$VC" box kill "$BOX" >/dev/null 2>&1
sleep 0.5
if [ -S "${XDG_RUNTIME_DIR:-/tmp}/vc-$BOX" ]; then
	bad "box kill removes the socket"
else
	ok "box kill removes the socket"
fi

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
