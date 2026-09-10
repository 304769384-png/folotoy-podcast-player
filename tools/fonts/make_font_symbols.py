#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate lv_font_conv --symbols strings for the two build-time fonts.

ui  : every CJK character used by the firmware source (fixed UI vocabulary),
      so the small 4 bpp font stays compact even as the UI evolves.
cjk : the complete GB2312 repertoire (6763 glyphs) plus every non-ASCII
      character found in the real podcast feed (future-proofs titles that use
      rare/GBK-only characters), for the 1 bpp fallback font.

Usage:
  make_font_symbols.py ui
  make_font_symbols.py cjk feed.xml
"""
import glob
import sys

CJK_RANGES = (
    (0x4E00, 0x9FFF),    # CJK unified ideographs
    (0x3400, 0x4DBF),    # extension A
    (0x3000, 0x303F),    # CJK symbols and punctuation
    (0xFF00, 0xFFEF),    # halfwidth/fullwidth forms
    (0x2018, 0x201F),    # smart quotes
    (0x2026, 0x2026),    # ellipsis
    (0x00B7, 0x00B7),    # middle dot
    (0x2260, 0x2260),    # not equal
)


def is_cjk(ch):
    cp = ord(ch)
    return any(lo <= cp <= hi for lo, hi in CJK_RANGES)


def ui_chars():
    chars = set()
    for pattern in ("main/*.cc", "main/*.h"):
        for path in glob.glob(pattern):
            with open(path, encoding="utf-8") as f:
                for ch in f.read():
                    if is_cjk(ch):
                        chars.add(ch)
    return chars


def gb2312_chars():
    chars = set()
    for b1 in range(0xB0, 0xF8):
        for b2 in range(0xA1, 0xFF):
            try:
                chars.add(bytes([b1, b2]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return chars


def main():
    mode = sys.argv[1]
    if mode == "ui":
        chars = ui_chars()
    elif mode == "cjk":
        chars = gb2312_chars()
        if len(sys.argv) > 2:
            with open(sys.argv[2], "rb") as f:
                text = f.read().decode("utf-8", "ignore")
            for ch in text:
                if ord(ch) > 0x7F and ch not in chars:
                    chars.add(ch)
    else:
        raise SystemExit("unknown mode")
    sys.stdout.write("".join(sorted(chars)))


if __name__ == "__main__":
    main()
