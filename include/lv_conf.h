// LVGL 8.3 configuration for tdeck-os. Only options that differ from LVGL's defaults are set.
#if 1  /* enable */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0            /* TFT_eSPI::pushColors(..., swap=true) swaps on the way out */

#define LV_MEM_SIZE (64U * 1024U)     /* internal RAM; PSRAM is reserved for later maps */

#define LV_DISP_DEF_REFR_PERIOD 30
#define LV_INDEV_DEF_READ_PERIOD 30

#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 0
#define LV_USE_ASSERT_MALLOC 0

#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_THEME_DEFAULT 0        /* all styling is explicit */

#define LV_USE_IMGFONT 1              /* inline emoji images from the SD card */
#define LV_IMG_CACHE_DEF_SIZE 24     /* small decoded-image cache, so emoji are not re-read per frame */

#endif /*LV_CONF_H*/
#endif
