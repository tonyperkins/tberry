#include "emoji.h"
#include "fonts/fonts.h"
#include <Arduino.h>
#include <SD.h>

static bool packPresent = false;
static lv_font_t* font14 = nullptr;
static lv_font_t* font12 = nullptr;
static constexpr int EMOJI_PX = 18;

// ── LVGL file system driver ('S:' = the SD card) ─────────────────
static void* fsOpen(lv_fs_drv_t*, const char* path, lv_fs_mode_t mode) {
  if (mode != LV_FS_MODE_RD) return nullptr;
  char full[96];
  snprintf(full, sizeof(full), "%s%s", path[0] == '/' ? "" : "/", path);
  File* f = new File(SD.open(full, FILE_READ));
  if (!f || !*f) { delete f; return nullptr; }
  return f;
}
static lv_fs_res_t fsClose(lv_fs_drv_t*, void* fp) {
  File* f = (File*)fp;
  f->close();
  delete f;
  return LV_FS_RES_OK;
}
static lv_fs_res_t fsRead(lv_fs_drv_t*, void* fp, void* buf, uint32_t btr, uint32_t* br) {
  *br = ((File*)fp)->read((uint8_t*)buf, btr);
  return LV_FS_RES_OK;
}
static lv_fs_res_t fsSeek(lv_fs_drv_t*, void* fp, uint32_t pos, lv_fs_whence_t whence) {
  SeekMode m = whence == LV_FS_SEEK_SET ? SeekSet : whence == LV_FS_SEEK_CUR ? SeekCur : SeekEnd;
  return ((File*)fp)->seek(pos, m) ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}
static lv_fs_res_t fsTell(lv_fs_drv_t*, void* fp, uint32_t* pos) {
  *pos = ((File*)fp)->position();
  return LV_FS_RES_OK;
}

// ── which code points we treat as emoji, and whether their file exists ──
static bool isEmojiRange(uint32_t cp) {
  return (cp >= 0x1F000 && cp <= 0x1FFFF) || (cp >= 0x2190 && cp <= 0x2BFF);
}
static int cacheIndex(uint32_t cp) {
  if (cp >= 0x1F000 && cp <= 0x1FFFF) return cp - 0x1F000;                      // 4096 entries
  if (cp >= 0x2190 && cp <= 0x2BFF) return 4096 + (cp - 0x2190);                // 2672 entries
  return -1;
}
// 2 bits per code point: 0 unknown, 1 present, 2 missing
static uint8_t seen[(4096 + 2672 + 3) / 4];

static bool hasImage(uint32_t cp) {
  int i = cacheIndex(cp);
  if (i < 0 || !packPresent) return false;
  uint8_t& b = seen[i / 4];
  int sh = (i % 4) * 2;
  int st = (b >> sh) & 3;
  if (st == 0) {
    char path[40];
    snprintf(path, sizeof(path), "/emoji/%x.bin", (unsigned)cp);
    st = SD.exists(path) ? 1 : 2;
    b = (b & ~(3 << sh)) | (st << sh);
  }
  return st == 1;
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

  memset(seen, 0, sizeof(seen));
  packPresent = SD.exists("/emoji");
  if (!packPresent) return;

  // Image fonts: emoji come from files, everything else falls through to the text font.
  font14 = lv_imgfont_create(EMOJI_PX, pathCb);
  if (font14) font14->fallback = &font_tdos_14;
  font12 = lv_imgfont_create(EMOJI_PX - 2, pathCb);
  if (font12) font12->fallback = &font_tdos_12;
}

bool emoji_pack_present() { return packPresent; }

const lv_font_t* emoji_font(const lv_font_t* text_font) {
  if (text_font == &font_tdos_14 && font14) return font14;
  if (text_font == &font_tdos_12 && font12) return font12;
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
      for (int k = 0; k < len && src[i + k] && o + 1 < n; k++) dst[o++] = src[i + k];
    }
    i += len;
  }
  dst[o] = 0;
}
