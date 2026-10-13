#pragma once
// UI fonts: Montserrat Medium (SIL Open Font License) with Latin-1, Latin Extended-A and common
// typographic punctuation. Symbol glyphs (LV_SYMBOL_*) come from LVGL's built-in font of the same
// size through the fallback pointer.
//
// Regenerate with lv_font_conv, e.g. for size 14:
//   npx lv_font_conv@1.5.3 --no-compress --no-prefilter --bpp 4 --size 14 \
//     --font Montserrat-Medium.ttf -r 0x20-0x7F,0xA0-0x17F,0x2010-0x2027,0x20AC,0x2122 \
//     --format lvgl --lv-font-name font_tdos_14 -o font_tdos_14.c --force-fast-kern-format
// then set `.fallback = &lv_font_montserrat_14`.
#include <lvgl.h>

LV_FONT_DECLARE(font_tdos_12)
LV_FONT_DECLARE(font_tdos_14)
LV_FONT_DECLARE(font_tdos_16)
LV_FONT_DECLARE(font_tdos_20)
