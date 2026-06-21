#!/usr/bin/env bash
# Regression suite for scr-tools.
#
# Usage: bash scripts/regression.sh ./scrtools
#
# The suite is self-contained: it synthesises a test screen from scratch, so it
# does not depend on any external image. It checks the two invariants that
# genuinely hold (the rendered image is the canonical truth):
#   1. png -> scr -> png is pixel-for-pixel identical (the real round-trip).
#   2. png2scr output is a canonical FIXED POINT: feeding it back through
#      scr -> png -> scr lands on the exact same bytes.
# Note: scr -> png -> scr is NOT byte-identical in general, because the render
# cannot distinguish ink/paper swaps or recover a colour a cell never paints.

set -euo pipefail

BIN="${1:-./scrtools}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0
ok()   { echo "  ok   - $1"; pass=$((pass+1)); }
bad()  { echo "  FAIL - $1"; fail=$((fail+1)); }

# --- Build a tiny known SCREEN$ entirely in the shell -------------------
# 6144 zero pixel bytes + 768 attribute bytes. We set a handful of cells to
# distinct ink/paper colours so the conversions have something to chew on.
python3 - "$TMP/seed.scr" <<'PY'
import sys
scr = bytearray(6912)
# Lay a simple gradient of attributes across the 768 cells.
for i in range(768):
    ink   = i % 8
    paper = (i // 8) % 8
    bright = (i // 64) & 1
    scr[6144 + i] = (bright << 6) | (paper << 3) | ink
# Sprinkle some set pixels so ink is actually exercised.
for off in range(0, 6144, 7):
    scr[off] = 0xA5
open(sys.argv[1], "wb").write(scr)
PY
[ "$(wc -c < "$TMP/seed.scr")" -eq 6912 ] && ok "seed scr is 6912 bytes" || bad "seed scr size"

# Render the seed once to obtain a canonical PNG we can round-trip.
"$BIN" scr2png "$TMP/seed.scr" "$TMP/p0.png" >/dev/null

# --- Test 1: png -> scr -> png is pixel-identical -----------------------
"$BIN" png2scr "$TMP/p0.png" "$TMP/s1.scr" >/dev/null
"$BIN" scr2png "$TMP/s1.scr" "$TMP/p1.png" >/dev/null
if cmp -s "$TMP/p0.png" "$TMP/p1.png"; then
    ok "png -> scr -> png is pixel-identical"
else
    bad "png -> scr -> png round-trip"
fi

# --- Test 2: png2scr output is a canonical fixed point ------------------
# Round-tripping the .scr again must reproduce the very same .scr bytes.
"$BIN" png2scr "$TMP/p1.png" "$TMP/s2.scr" >/dev/null
if cmp -s "$TMP/s1.scr" "$TMP/s2.scr"; then
    ok "png2scr output is a stable fixed point"
else
    bad "png2scr fixed-point stability"
fi

# --- Test 3: info reports the standard mode -----------------------------
"$BIN" info "$TMP/seed.scr" | grep -q "Mode: standard" \
    && ok "info recognises standard SCREEN\$" || bad "info standard detection"

# --- Test 4: Timex hi-colour round-trip (12288 bytes) -------------------
# Reuse p0.png (a valid 256x192 image) as a hi-colour source.
"$BIN" png2scr --mode hicolour "$TMP/p0.png" "$TMP/hc1.scr" >/dev/null
[ "$(wc -c < "$TMP/hc1.scr")" -eq 12288 ] && ok "hi-colour is 12288 bytes" || bad "hi-colour size"
"$BIN" info "$TMP/hc1.scr" | grep -q "Mode: Timex hi-colour" \
    && ok "info recognises hi-colour" || bad "hi-colour detection"
"$BIN" scr2png "$TMP/hc1.scr" "$TMP/hc1.png" >/dev/null
"$BIN" png2scr --mode hicolour "$TMP/hc1.png" "$TMP/hc2.scr" >/dev/null
cmp -s "$TMP/hc1.scr" "$TMP/hc2.scr" \
    && ok "hi-colour png2scr is a stable fixed point" || bad "hi-colour fixed point"

# --- Test 5: Timex hi-res round-trip (12289 bytes) ----------------------
# Synthesise a two-colour 512x192 image (vertical stripes) for hi-res.
python3 - "$TMP/hr.png" <<'PY'
import sys
from PIL import Image
im=Image.new("RGB",(512,192))
ink=(255,255,0); paper=(0,0,255)         # yellow on blue (a legal hi-res pair)
px=im.load()
for y in range(192):
    for x in range(512):
        px[x,y]= ink if ((x//4)%2==0) else paper
im.save(sys.argv[1])
PY
"$BIN" png2scr --mode hires "$TMP/hr.png" "$TMP/hr1.scr" >/dev/null
[ "$(wc -c < "$TMP/hr1.scr")" -eq 12289 ] && ok "hi-res is 12289 bytes" || bad "hi-res size"
"$BIN" info "$TMP/hr1.scr" | grep -q "Mode: Timex hi-res" \
    && ok "info recognises hi-res" || bad "hi-res detection"
"$BIN" scr2png "$TMP/hr1.scr" "$TMP/hr1.png" >/dev/null
"$BIN" png2scr --mode hires "$TMP/hr1.png" "$TMP/hr2.scr" >/dev/null
cmp -s "$TMP/hr1.scr" "$TMP/hr2.scr" \
    && ok "hi-res png2scr is a stable fixed point" || bad "hi-res fixed point"

# --- Test 6: scr2gif produces an animated GIF for a flashing screen -----
# The seed sets no FLASH bits, so force a couple on a copy.
python3 - "$TMP/seed.scr" "$TMP/flash.scr" <<'PY'
import sys
d=bytearray(open(sys.argv[1],"rb").read())
for i in range(10): d[6144+i] |= 0x80      # set FLASH on first 10 cells
open(sys.argv[2],"wb").write(d)
PY
"$BIN" scr2gif "$TMP/flash.scr" "$TMP/flash.gif" >/dev/null
python3 - "$TMP/flash.gif" <<'PY'
import sys
from PIL import Image
im=Image.open(sys.argv[1])
n=getattr(im,"n_frames",1)
print("frames",n)
sys.exit(0 if n==2 else 1)
PY
[ $? -eq 0 ] && ok "scr2gif emits a 2-frame animation for flashing screens" \
    || bad "scr2gif animation"

echo
echo "Passed: $pass  Failed: $fail"
[ "$fail" -eq 0 ]
