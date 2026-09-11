#pragma once

#include "lvgl.h"

// Both fonts are generated at build time by tools/fonts/generate_fonts.sh via
// lv_font_conv from Noto Sans SC, so this repo ships no binary font tables.
//
// pod_font      16 px / 4 bpp - ASCII plus the fixed interface vocabulary.
// pod_font_cjk  16 px / 1 bpp - GB2312 coverage (+ feed-titled glyphs), used as
//                               the LVGL fallback for arbitrary episode titles.
//
// pod_font is intentionally NON-const: LVGL 9.x attaches CJK fallback via the
// mutable `.fallback` field (no lv_font_set_fallback() exists in 9.x), so the
// generated definition is post-processed to drop `const`. pod_font_cjk stays
// const (it is only ever read as a fallback target).
extern lv_font_t pod_font;
LV_FONT_DECLARE(pod_font_cjk);
