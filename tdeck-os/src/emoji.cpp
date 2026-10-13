#include "emoji.h"
#include "fonts/fonts.h"
#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>

static bool packPresent = false;
static lv_font_t* font14 = nullptr;
static lv_font_t* font12 = nullptr;
static lv_font_t* font16 = nullptr;

// Joiners, variation selectors, skin tones and tags have no glyph and no image: draw them as
// nothing instead of LVGL's placeholder rectangle (names are shown raw, so they can hold them).
static bool zeroWidthDsc(const lv_font_t*, lv_font_glyph_dsc_t* d, uint32_t c, uint32_t) {
  bool zw = c == 0xFE0F || c == 0xFE0E || c == 0x200D || (c >= 0x1F3FB && c <= 0x1F3FF) || (c >= 0xE0000 && c <= 0xE007F);
  if (!zw) return false;
  d->adv_w = 0; d->box_w = 0; d->box_h = 0; d->ofs_x = 0; d->ofs_y = 0; d->bpp = 1;
  return true;
}
static const uint8_t* zeroWidthBitmap(const lv_font_t*, uint32_t) { return nullptr; }
static lv_font_t zw12, zw14, zw16;
static void makeZeroWidth(lv_font_t& z, const lv_font_t& text) {
  z = text;                                      // metrics, and the text font as the next fallback
  z.get_glyph_dsc = zeroWidthDsc;
  z.get_glyph_bitmap = zeroWidthBitmap;
  z.fallback = &text;
}     // the 14 px emoji images next to 16 pt text (compose box)

// ── In-memory cache of emoji files ───────────────────────────────
// Finding one file in a folder of 1400+ entries means a linear scan of the FAT directory, which is
// tens of milliseconds, and LVGL asks for the same image several times per redraw. So every emoji
// is read from the card once, kept in PSRAM, and served from memory after that. Negative results
// are remembered too, so a missing emoji costs one lookup ever.
struct Entry { uint32_t cp; uint8_t* data; uint16_t len; };
static constexpr int CACHE_N = 384;
static Entry cache[CACHE_N];
static int cacheCount = 0;

static Entry* lookup(uint32_t cp) {
  for (int i = 0; i < cacheCount; i++)
    if (cache[i].cp == cp) return cache[i].data ? &cache[i] : nullptr;
  if (cacheCount >= CACHE_N) return nullptr;       // full: further new emoji are left out
  Entry& e = cache[cacheCount++];
  e.cp = cp; e.data = nullptr; e.len = 0;
  char path[40];
  snprintf(path, sizeof(path), "/emoji/%x.bin", (unsigned)cp);
  File f = SD.open(path, FILE_READ);
  if (f) {
    size_t n = f.size();
    if (n > 0 && n <= 2048) {
      e.data = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
      if (!e.data) e.data = (uint8_t*)malloc(n);
      if (e.data && f.read(e.data, n) == n) e.len = (uint16_t)n;
      else { free(e.data); e.data = nullptr; }
    }
    f.close();
  }
  return e.data ? &e : nullptr;
}

// ── LVGL file system driver ('S:' = the emoji cache) ─────────────
struct MemFile { const uint8_t* data; uint32_t len, pos; };

static void* fsOpen(lv_fs_drv_t*, const char* path, lv_fs_mode_t mode) {
  if (mode != LV_FS_MODE_RD || !packPresent) return nullptr;
  const char* base = strrchr(path, '/');
  unsigned cp = 0;
  if (!base || sscanf(base + 1, "%x", &cp) != 1) return nullptr;
  Entry* e = lookup(cp);
  if (!e) return nullptr;
  return new MemFile{ e->data, e->len, 0 };
}
static lv_fs_res_t fsClose(lv_fs_drv_t*, void* fp) {
  delete (MemFile*)fp;
  return LV_FS_RES_OK;
}
static lv_fs_res_t fsRead(lv_fs_drv_t*, void* fp, void* buf, uint32_t btr, uint32_t* br) {
  MemFile* f = (MemFile*)fp;
  uint32_t n = f->pos >= f->len ? 0 : min<uint32_t>(btr, f->len - f->pos);
  memcpy(buf, f->data + f->pos, n);
  f->pos += n;
  *br = n;
  return LV_FS_RES_OK;
}
static lv_fs_res_t fsSeek(lv_fs_drv_t*, void* fp, uint32_t pos, lv_fs_whence_t whence) {
  MemFile* f = (MemFile*)fp;
  int64_t np = whence == LV_FS_SEEK_SET ? (int64_t)pos : whence == LV_FS_SEEK_CUR ? (int64_t)f->pos + pos : (int64_t)f->len + pos;
  if (np < 0 || np > f->len) return LV_FS_RES_UNKNOWN;
  f->pos = (uint32_t)np;
  return LV_FS_RES_OK;
}
static lv_fs_res_t fsTell(lv_fs_drv_t*, void* fp, uint32_t* pos) {
  *pos = ((MemFile*)fp)->pos;
  return LV_FS_RES_OK;
}

// ── which code points we treat as emoji ──────────────────────────
static bool isEmojiRange(uint32_t cp) {
  return (cp >= 0x1F000 && cp <= 0x1FFFF) || (cp >= 0x2190 && cp <= 0x2BFF);
}
static bool hasImage(uint32_t cp) {
  return packPresent && isEmojiRange(cp) && lookup(cp) != nullptr;
}

static bool pathCb(const lv_font_t*, void* img_src, uint16_t len, uint32_t unicode, uint32_t) {
  if (!hasImage(unicode)) return false;
  snprintf((char*)img_src, len, "S:/emoji/%x.bin", (unsigned)unicode);
  return true;
}

void emoji_init() {
  static lv_fs_drv_t drv;
  lv_fs_drv_init(&drv);
  drv.letter = 'S';
  drv.open_cb = fsOpen;
  drv.close_cb = fsClose;
  drv.read_cb = fsRead;
  drv.seek_cb = fsSeek;
  drv.tell_cb = fsTell;
  lv_fs_drv_register(&drv);

  cacheCount = 0;
  packPresent = SD.exists("/emoji");
  if (!packPresent) return;

  // Image fonts: emoji come from files, everything else falls through to the text font.
  // The label takes its line height and baseline from the font it is given, so the image font
  // copies them from the text font: images then sit on the same baseline as the letters.
  font14 = lv_imgfont_create(font_tdos_14.line_height, pathCb);
  if (font14) {
    makeZeroWidth(zw14, font_tdos_14);
    font14->fallback = &zw14;
    font14->base_line = font_tdos_14.base_line;
    font14->line_height = font_tdos_14.line_height;
  }
  font12 = lv_imgfont_create(font_tdos_12.line_height, pathCb);
  if (font12) {
    makeZeroWidth(zw12, font_tdos_12);
    font12->fallback = &zw12;
    font12->base_line = font_tdos_12.base_line;
    font12->line_height = font_tdos_12.line_height;
  }
  font16 = lv_imgfont_create(font_tdos_16.line_height, pathCb);
  if (font16) {
    makeZeroWidth(zw16, font_tdos_16);
    font16->fallback = &zw16;
    font16->base_line = font_tdos_16.base_line;
    font16->line_height = font_tdos_16.line_height;
  }
}

bool emoji_pack_present() { return packPresent; }

const lv_font_t* emoji_font(const lv_font_t* text_font) {
  if (text_font == &font_tdos_14 && font14) return font14;
  if (text_font == &font_tdos_12 && font12) return font12;
  if (text_font == &font_tdos_16 && font16) return font16;
  return text_font;
}

void emoji_filter(const char* src, char* dst, size_t n, bool keepAvailable) {
  size_t o = 0;
  for (size_t i = 0; src[i] && o + 1 < n;) {
    uint8_t c = (uint8_t)src[i];
    int len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    uint32_t cp = c;
    if (len > 1) {
      cp = len == 4 ? (c & 0x07) : len == 3 ? (c & 0x0F) : (c & 0x1F);
      for (int k = 1; k < len && src[i + k]; k++) cp = (cp << 6) | ((uint8_t)src[i + k] & 0x3F);
    }
    bool keep = !isEmojiRange(cp) || (keepAvailable && hasImage(cp));
    if (keep) {
      if (o + len >= n) break;                     // no room for the whole character: stop, don't cut it
      for (int k = 0; k < len && src[i + k]; k++) dst[o++] = src[i + k];
    }
    i += len;
  }
  dst[o] = 0;
}
