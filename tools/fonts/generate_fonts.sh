#!/usr/bin/env bash
# Generates main/pod_font.c (UI, 4bpp) and main/pod_font_cjk.c (GB2312+titles,
# 1bpp fallback) with lv_font_conv. Run from the repository root, e.g. in CI:
#
#   FEED_XML=/tmp/feed.xml bash tools/fonts/generate_fonts.sh
#
# Re-running regenerates both files; they are committed only as build artifacts
# of CI (local builds can run this script too if node/npm are available).
set -euo pipefail

FONT_DIR="tools/fonts"
FONT_FILE="$FONT_DIR/NotoSansSC-VF.ttf"
FONT_URL="https://github.com/google/fonts/raw/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf"

if [ ! -f "$FONT_FILE" ]; then
  echo "Downloading Noto Sans SC..."
  curl -fsSL --retry 3 -o "$FONT_FILE" "$FONT_URL"
fi

UI_SYMBOLS="$(python3 "$FONT_DIR/make_font_symbols.py" ui)"
echo "UI font glyphs (CJK): $(printf '%s' "$UI_SYMBOLS" | python3 -c 'import sys; print(len(sys.stdin.read()))')"

npx --yes lv_font_conv@1.5.3 \
  --font "$FONT_FILE" \
  --range 0x20-0x7F \
  --symbols "$UI_SYMBOLS" \
  --size 16 --format lvgl --bpp 4 --no-compress \
  --lv-include lvgl.h --lv-font-name pod_font \
  -o main/pod_font.c

# LVGL 9.x has no lv_font_set_fallback(); the CJK fallback is wired via the
# mutable `.fallback` field. pod_font must be non-const for that (see
# main/pod_font.h). lv_font_conv emits `const lv_font_t pod_font`, so strip it.
sed -i 's/^const lv_font_t pod_font = {$/lv_font_t pod_font = {/' main/pod_font.c

if [ -n "${FEED_XML:-}" ] && [ -f "$FEED_XML" ]; then
  CJK_SYMBOLS="$(python3 "$FONT_DIR/make_font_symbols.py" cjk "$FEED_XML")"
else
  echo "FEED_XML not provided; CJK fallback covers GB2312 only"
  CJK_SYMBOLS="$(python3 "$FONT_DIR/make_font_symbols.py" cjk)"
fi
echo "CJK fallback glyphs: $(printf '%s' "$CJK_SYMBOLS" | python3 -c 'import sys; print(len(sys.stdin.read()))')"

npx --yes lv_font_conv@1.5.3 \
  --font "$FONT_FILE" \
  --symbols "$CJK_SYMBOLS" \
  --size 16 --format lvgl --bpp 1 --no-compress \
  --lv-include lvgl.h --lv-font-name pod_font_cjk \
  -o main/pod_font_cjk.c

echo "Fonts generated:"
wc -c main/pod_font.c main/pod_font_cjk.c
