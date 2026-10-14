#include "mapview.h"
#include "nodes.h"
#include "fonts/fonts.h"
#include "emoji.h"
#include <Arduino.h>
#include <SD.h>
#include <PNGdec.h>
#include <esp_heap_caps.h>
#include <math.h>

static constexpr int VIEW_W = 320, VIEW_H = 218;       // below the 22 px status bar
static constexpr int VIEW_Y = 22;
static constexpr int TILE = 256;
static constexpr int MIN_Z = 6, MAX_Z = 16;
static constexpr int CACHE_TILES = 14;                 // 14 x 128 KB of PSRAM
static constexpr int MAX_MARKERS = 24;
static constexpr uint16_t BG565 = 0x0861;              // close to the UI's ink colour

static const uint32_t ACCENT = 0xFFB547, INK = 0x0A0E14, TEXT = 0xE8EDF3, MUTE = 0x8593A5, RAISE = 0x131A24;

// ── tile cache ───────────────────────────────────────────────────
struct CacheEntry { int16_t z; int32_t x, y; uint16_t* pix; uint32_t used; bool missing; bool valid; };
static CacheEntry cache[CACHE_TILES];
static uint32_t useClock = 0;

// ── PNG decoding (PNGdec needs C-style file callbacks) ───────────
static PNG* png = nullptr;
static File tileFile;
static uint16_t* decodeDst = nullptr;
static void* pngOpen(const char* name, int32_t* size) {
  tileFile = SD.open(name, FILE_READ);
  if (!tileFile) return nullptr;
  *size = tileFile.size();
  return &tileFile;
}
static void pngClose(void*) { tileFile.close(); }
static int32_t pngRead(PNGFILE*, uint8_t* buf, int32_t len) { return tileFile.read(buf, len); }
static int32_t pngSeek(PNGFILE*, int32_t pos) { return tileFile.seek(pos); }
static int pngDraw(PNGDRAW* d) {
  if (d->y < TILE && decodeDst) png->getLineAsRGB565(d, decodeDst + d->y * TILE, PNG_RGB565_LITTLE_ENDIAN, 0xFFFFFFFF);
  return 1;
}

static bool decodeTile(int z, int x, int y, uint16_t* dst) {
  if (!png) {
    png = (PNG*)heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM);
    if (!png) png = (PNG*)malloc(sizeof(PNG));
    if (!png) return false;
    new (png) PNG();
  }
  char path[40];
  snprintf(path, sizeof(path), "/maps/%d/%d/%d.png", z, x, y);
  decodeDst = dst;
  int rc = png->open(path, pngOpen, pngClose, pngRead, pngSeek, pngDraw);
  if (rc != PNG_SUCCESS) return false;
  if (png->getWidth() != TILE || png->getHeight() != TILE) { png->close(); return false; }
  rc = png->decode(nullptr, 0);
  png->close();
  return rc == PNG_SUCCESS;
}

// Returns the tile's pixels, or nullptr if it is not on the card (or the budget is spent).
static int decodeBudget = 0;
static bool deferred = false;       // some tile wanted decoding but the budget ran out
static uint16_t* getTile(int z, int x, int y, bool allowDecode) {
  for (auto& e : cache)
    if (e.valid && e.z == z && e.x == x && e.y == y) { e.used = ++useClock; return e.missing ? nullptr : e.pix; }
  if (!allowDecode) return nullptr;
  if (decodeBudget <= 0) { deferred = true; return nullptr; }
  decodeBudget--;

  CacheEntry* victim = &cache[0];
  for (auto& e : cache) {
    if (!e.valid) { victim = &e; break; }
    if (e.used < victim->used) victim = &e;
  }
  if (!victim->pix) victim->pix = (uint16_t*)heap_caps_malloc(TILE * TILE * 2, MALLOC_CAP_SPIRAM);
  if (!victim->pix) return nullptr;
  victim->z = z; victim->x = x; victim->y = y;
  victim->used = ++useClock;
  victim->valid = true;
  victim->missing = !decodeTile(z, x, y, victim->pix);
  return victim->missing ? nullptr : victim->pix;
}

// ── view state ───────────────────────────────────────────────────
static lv_obj_t *canvas, *info, *hintLbl;
static uint16_t* canvasBuf = nullptr;
static int zoom = 13;
static double cx = 0, cy = 0;               // view centre in world pixels at `zoom`
static bool haveOwn = false;
static double ownLat = 0, ownLon = 0;
static bool followMe = true;                // keep centred on our own position until the user pans
static bool dirty = true;
static uint32_t lastRenderMs = 0;
static bool active = false;

struct Marker { lv_obj_t* dot; lv_obj_t* name; };
static Marker markers[MAX_MARKERS];
static lv_obj_t* meDot = nullptr;
// A place the user asked to see (TDOS-004): where a message came from.
static bool focusOn = false;
static double focusLat = 0, focusLon = 0;
static char focusName[40] = "";
static lv_obj_t *focusRing = nullptr, *focusLbl = nullptr;

// Great-circle distance in km and compass point of the bearing from (a) to (b).
static double distanceKm(double la1, double lo1, double la2, double lo2, const char** dir) {
  const double R = 6371.0, d2r = M_PI / 180.0;
  double p1 = la1 * d2r, p2 = la2 * d2r, dp = (la2 - la1) * d2r, dl = (lo2 - lo1) * d2r;
  double h = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
  double dist = 2 * R * asin(sqrt(h));
  double y = sin(dl) * cos(p2), x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
  double brg = fmod(atan2(y, x) / d2r + 360.0, 360.0);
  static const char* const pts[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
  if (dir) *dir = pts[(int)((brg + 22.5) / 45.0) % 8];
  return dist;
}

static double worldSize(int z) { return (double)TILE * (double)(1UL << z); }
static void toWorld(double lat, double lon, int z, double& x, double& y) {
  double n = worldSize(z);
  x = (lon + 180.0) / 360.0 * n;
  double s = sin(lat * M_PI / 180.0);
  y = (0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI)) * n;
}
static void fromWorld(double x, double y, int z, double& lat, double& lon) {
  double n = worldSize(z);
  lon = x / n * 360.0 - 180.0;
  lat = atan(sinh(M_PI * (1.0 - 2.0 * y / n))) * 180.0 / M_PI;
}

// ── drawing ──────────────────────────────────────────────────────
static void fillBackground() {
  for (int i = 0; i < VIEW_W * VIEW_H; i++) canvasBuf[i] = BG565;
}

// Copy one tile (or a magnified part of a coarser one) into the canvas at (ox, oy).
static void drawTile(int z, int tx, int ty, int ox, int oy) {
  // 1. the tile itself
  uint16_t* t = getTile(z, tx, ty, true);
  int shift = 0, sx = 0, sy = 0;
  if (!t) {
    // 2. not on the card (or not decoded yet): use a coarser tile, magnified
    for (int k = 1; k <= 4 && z - k >= MIN_Z && !t; k++) {
      uint16_t* p = getTile(z - k, tx >> k, ty >> k, true);
      if (p) {
        t = p; shift = k;
        int sub = TILE >> k;
        sx = (tx & ((1 << k) - 1)) * sub;
        sy = (ty & ((1 << k) - 1)) * sub;
      }
    }
    if (!t) return;
  }
  for (int r = 0; r < TILE; r++) {
    int dy = oy + r;
    if (dy < 0 || dy >= VIEW_H) continue;
    int c0 = max(0, -ox), c1 = min(TILE, VIEW_W - ox);
    if (c0 >= c1) continue;
    uint16_t* dst = canvasBuf + dy * VIEW_W + ox;
    if (shift == 0) {
      memcpy(dst + c0, t + r * TILE + c0, (c1 - c0) * 2);
    } else {
      const uint16_t* srow = t + (sy + (r >> shift)) * TILE + sx;
      for (int c = c0; c < c1; c++) dst[c] = srow[c >> shift];
    }
  }
}

static void placeMarkers();

static void render() {
  if (!canvasBuf) return;
  decodeBudget = 3;                         // a few decodes per pass keeps the UI responsive
  deferred = false;
  fillBackground();
  double left = cx - VIEW_W / 2.0, top = cy - VIEW_H / 2.0;
  int n = 1 << zoom;
  int tx0 = (int)floor(left / TILE), tx1 = (int)floor((left + VIEW_W - 1) / TILE);
  int ty0 = (int)floor(top / TILE), ty1 = (int)floor((top + VIEW_H - 1) / TILE);
  for (int ty = ty0; ty <= ty1; ty++) {
    if (ty < 0 || ty >= n) continue;
    for (int tx = tx0; tx <= tx1; tx++) {
      if (tx < 0 || tx >= n) continue;
      drawTile(zoom, tx, ty, (int)lround(tx * (double)TILE - left), (int)lround(ty * (double)TILE - top));
    }
  }
  lv_obj_invalidate(canvas);
  placeMarkers();
  dirty = deferred;                         // more to load: the next tick continues
  lastRenderMs = millis();

  double lat, lon;
  fromWorld(cx, cy, zoom, lat, lon);
  char b[96];
  if (focusOn && haveOwn) {
    const char* dir = "";
    double km = distanceKm(ownLat, ownLon, focusLat, focusLon, &dir);
    if (km < 1.0) snprintf(b, sizeof(b), "%.0f m %s of you  z%d", km * 1000.0, dir, zoom);
    else snprintf(b, sizeof(b), "%.1f km %s of you  z%d", km, dir, zoom);
  } else if (focusOn) {
    snprintf(b, sizeof(b), "no GPS fix for distance  z%d", zoom);
  } else {
    snprintf(b, sizeof(b), "z%d  %.4f, %.4f", zoom, lat, lon);
  }
  lv_label_set_text(info, b);
}

static void setMarker(int i, double lat, double lon, uint32_t colour, const char* name) {
  if (i >= MAX_MARKERS || !markers[i].dot) return;
  double wx, wy;
  toWorld(lat, lon, zoom, wx, wy);
  int x = (int)lround(wx - (cx - VIEW_W / 2.0));
  int y = (int)lround(wy - (cy - VIEW_H / 2.0));
  bool visible = x > -8 && x < VIEW_W + 8 && y > -8 && y < VIEW_H + 8;
  if (!visible) {
    lv_obj_add_flag(markers[i].dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(markers[i].name, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_set_style_bg_color(markers[i].dot, lv_color_hex(colour), 0);
  lv_obj_set_pos(markers[i].dot, x - 5, VIEW_Y + y - 5);
  lv_obj_clear_flag(markers[i].dot, LV_OBJ_FLAG_HIDDEN);
  char shown[64];
  emoji_filter(name, shown, sizeof(shown), true);
  lv_label_set_text(markers[i].name, shown);
  lv_obj_set_pos(markers[i].name, x + 8, VIEW_Y + y - 8);
  lv_obj_clear_flag(markers[i].name, LV_OBJ_FLAG_HIDDEN);
}

static void placeMarkers() {
  // this device
  if (meDot) {
    if (haveOwn) {
      double wx, wy;
      toWorld(ownLat, ownLon, zoom, wx, wy);
      int x = (int)lround(wx - (cx - VIEW_W / 2.0));
      int y = (int)lround(wy - (cy - VIEW_H / 2.0));
      lv_obj_set_pos(meDot, x - 8, VIEW_Y + y - 8);
      if (x > -16 && x < VIEW_W + 16 && y > -16 && y < VIEW_H + 16) lv_obj_clear_flag(meDot, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(meDot, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(meDot, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (focusRing && focusLbl) {
    if (focusOn) {
      double wx, wy;
      toWorld(focusLat, focusLon, zoom, wx, wy);
      int x = (int)lround(wx - (cx - VIEW_W / 2.0));
      int y = (int)lround(wy - (cy - VIEW_H / 2.0));
      bool vis = x > -20 && x < VIEW_W + 20 && y > -20 && y < VIEW_H + 20;
      if (vis) {
        lv_obj_set_pos(focusRing, x - 12, VIEW_Y + y - 12);
        char shown[96];
        emoji_filter(focusName, shown, sizeof(shown), true);
        lv_label_set_text(focusLbl, shown);
        lv_obj_set_pos(focusLbl, x + 14, VIEW_Y + y - 8);
        lv_obj_clear_flag(focusRing, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(focusLbl, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(focusRing, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(focusLbl, LV_OBJ_FLAG_HIDDEN);
      }
    } else {
      lv_obj_add_flag(focusRing, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(focusLbl, LV_OBJ_FLAG_HIDDEN);
    }
  }
  // other nodes that advertised a position
  int m = 0;
  int total = nodes_count();
  for (int i = 0; i < total && m < MAX_MARKERS; i++) {
    const NodeInfo* n = nodes_sorted(i);
    if (!n || (!n->lat && !n->lon)) continue;
    uint32_t col = n->type == 2 ? 0x2FC4B2 : n->type == 3 ? 0x8B7CFF : n->type == 4 ? 0x43C77B : ACCENT;
    setMarker(m++, n->lat / 1e6, n->lon / 1e6, col, n->name);
  }
  for (; m < MAX_MARKERS; m++) setMarker(m, 0, 0, 0, "");   // hide the unused ones
  for (int i = 0; i < MAX_MARKERS; i++) {                    // setMarker() above skipped them as hidden
    if (i >= m) {
      if (markers[i].dot) lv_obj_add_flag(markers[i].dot, LV_OBJ_FLAG_HIDDEN);
      if (markers[i].name) lv_obj_add_flag(markers[i].name, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

// ── events ───────────────────────────────────────────────────────
static void dragCb(lv_event_t*) {
  lv_indev_t* in = lv_indev_get_act();
  if (!in) return;
  lv_point_t v;
  lv_indev_get_vect(in, &v);
  if (v.x == 0 && v.y == 0) return;
  cx -= v.x; cy -= v.y;
  followMe = false;
  if (millis() - lastRenderMs > 60) render();      // follows the finger without piling up work
  else dirty = true;
}
static void zoomInCb(lv_event_t*) { mapview_zoom(1); }
static void zoomOutCb(lv_event_t*) { mapview_zoom(-1); }
static void meCb(lv_event_t*) { mapview_centre_on_me(); }

static lv_obj_t* roundButton(lv_obj_t* parent, const char* text, int y, lv_event_cb_t cb) {
  lv_obj_t* b = lv_obj_create(parent);
  lv_obj_set_size(b, 34, 34);
  lv_obj_set_pos(b, 282, y);
  lv_obj_set_style_bg_color(b, lv_color_hex(RAISE), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_90, 0);
  lv_obj_set_style_radius(b, 17, 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, lv_color_hex(0x243041), 0);
  lv_obj_set_style_pad_all(b, 0, 0);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, &font_tdos_16, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(TEXT), 0);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  return b;
}

// ── public ───────────────────────────────────────────────────────
void mapview_create(lv_obj_t* parent) {
  active = true;
  if (!canvasBuf) canvasBuf = (uint16_t*)heap_caps_malloc(VIEW_W * VIEW_H * 2, MALLOC_CAP_SPIRAM);

  canvas = lv_canvas_create(parent);
  lv_canvas_set_buffer(canvas, canvasBuf, VIEW_W, VIEW_H, LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_pos(canvas, 0, VIEW_Y);
  lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(canvas, dragCb, LV_EVENT_PRESSING, nullptr);

  for (int i = 0; i < MAX_MARKERS; i++) {
    lv_obj_t* d = lv_obj_create(parent);
    lv_obj_set_size(d, 10, 10);
    lv_obj_set_style_radius(d, 5, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d, 2, 0);
    lv_obj_set_style_border_color(d, lv_color_hex(INK), 0);
    lv_obj_set_style_pad_all(d, 0, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* n = lv_label_create(parent);
    lv_label_set_text(n, "");
    lv_obj_set_style_text_font(n, emoji_font(&font_tdos_12), 0);
    lv_obj_set_style_text_color(n, lv_color_hex(TEXT), 0);
    lv_obj_set_style_bg_color(n, lv_color_hex(INK), 0);
    lv_obj_set_style_bg_opa(n, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(n, 3, 0);
    lv_obj_set_style_radius(n, 4, 0);
    lv_obj_clear_flag(n, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(n, LV_OBJ_FLAG_HIDDEN);
    markers[i] = { d, n };
  }

  // the focus marker: an amber ring, much more noticeable than the node dots
  focusRing = lv_obj_create(parent);
  lv_obj_set_size(focusRing, 24, 24);
  lv_obj_set_style_radius(focusRing, 12, 0);
  lv_obj_set_style_bg_opa(focusRing, LV_OPA_20, 0);
  lv_obj_set_style_bg_color(focusRing, lv_color_hex(ACCENT), 0);
  lv_obj_set_style_border_width(focusRing, 3, 0);
  lv_obj_set_style_border_color(focusRing, lv_color_hex(ACCENT), 0);
  lv_obj_set_style_pad_all(focusRing, 0, 0);
  lv_obj_clear_flag(focusRing, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(focusRing, LV_OBJ_FLAG_HIDDEN);
  focusLbl = lv_label_create(parent);
  lv_label_set_text(focusLbl, "");
  lv_obj_set_style_text_font(focusLbl, emoji_font(&font_tdos_12), 0);
  lv_obj_set_style_text_color(focusLbl, lv_color_hex(ACCENT), 0);
  lv_obj_set_style_bg_color(focusLbl, lv_color_hex(INK), 0);
  lv_obj_set_style_bg_opa(focusLbl, LV_OPA_80, 0);
  lv_obj_set_style_pad_hor(focusLbl, 3, 0);
  lv_obj_set_style_radius(focusLbl, 4, 0);
  lv_obj_clear_flag(focusLbl, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(focusLbl, LV_OBJ_FLAG_HIDDEN);
  focusOn = false;

  // our own position: a larger blue dot with a white ring
  meDot = lv_obj_create(parent);
  lv_obj_set_size(meDot, 16, 16);
  lv_obj_set_style_radius(meDot, 8, 0);
  lv_obj_set_style_bg_color(meDot, lv_color_hex(0x4C8DFF), 0);
  lv_obj_set_style_bg_opa(meDot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(meDot, 3, 0);
  lv_obj_set_style_border_color(meDot, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_pad_all(meDot, 0, 0);
  lv_obj_clear_flag(meDot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(meDot, LV_OBJ_FLAG_HIDDEN);

  roundButton(parent, "+", 50, zoomInCb);
  roundButton(parent, "-", 90, zoomOutCb);
  roundButton(parent, LV_SYMBOL_GPS, 130, meCb);

  info = lv_label_create(parent);
  lv_obj_set_style_text_font(info, &font_tdos_12, 0);
  lv_obj_set_style_text_color(info, lv_color_hex(TEXT), 0);
  lv_obj_set_style_bg_color(info, lv_color_hex(INK), 0);
  lv_obj_set_style_bg_opa(info, LV_OPA_70, 0);
  lv_obj_set_style_pad_hor(info, 4, 0);
  lv_obj_set_style_radius(info, 4, 0);
  lv_obj_set_pos(info, 4, 240 - 20);
  lv_label_set_text(info, "");

  hintLbl = lv_label_create(parent);                   // required attribution for OpenStreetMap data
  lv_obj_set_style_text_font(hintLbl, &font_tdos_12, 0);
  lv_obj_set_style_text_color(hintLbl, lv_color_hex(MUTE), 0);
  lv_obj_set_style_bg_color(hintLbl, lv_color_hex(INK), 0);
  lv_obj_set_style_bg_opa(hintLbl, LV_OPA_70, 0);
  lv_obj_set_style_pad_hor(hintLbl, 3, 0);
  lv_label_set_text(hintLbl, "\xC2\xA9 OpenStreetMap");
  lv_obj_set_pos(hintLbl, 320 - 100, 240 - 20);

  // Starting view: our position if we have one, else the centre of the region.
  if (cx == 0 && cy == 0) {
    toWorld(30.2672, -97.7431, zoom, cx, cy);
  }
  if (haveOwn && followMe) toWorld(ownLat, ownLon, zoom, cx, cy);
  dirty = true;
  render();
}

void mapview_destroy() {
  active = false;
  canvas = nullptr; info = nullptr; hintLbl = nullptr; meDot = nullptr; focusRing = nullptr; focusLbl = nullptr;
  for (auto& m : markers) m = { nullptr, nullptr };
}

bool mapview_active() { return active; }

void mapview_update(const UiModel& m) {
  bool had = haveOwn;
  if (m.hasPos) {
    ownLat = m.latE6 / 1e6;
    ownLon = m.lonE6 / 1e6;
    haveOwn = true;
  }
  if (!active) return;
  if (haveOwn && followMe) {
    double nx, ny;
    toWorld(ownLat, ownLon, zoom, nx, ny);
    if (fabs(nx - cx) > 1.0 || fabs(ny - cy) > 1.0 || !had) { cx = nx; cy = ny; dirty = true; }
  }
  static uint32_t seenNodes = 0;
  if (nodes_version() != seenNodes) { seenNodes = nodes_version(); dirty = true; }   // a node moved or appeared
}

void mapview_tick() {
  if (!active || !dirty || !canvasBuf) return;
  if (millis() - lastRenderMs < 120) return;
  render();
}

void mapview_pan(int dx, int dy) {
  cx += dx; cy += dy;
  followMe = false;
  dirty = true;
}

void mapview_zoom(int delta) {
  int nz = constrain(zoom + delta, MIN_Z, MAX_Z);
  if (nz == zoom) return;
  double lat, lon;
  fromWorld(cx, cy, zoom, lat, lon);       // keep the same point at the centre
  zoom = nz;
  toWorld(lat, lon, zoom, cx, cy);
  dirty = true;
}

void mapview_focus(double lat, double lon, const char* name) {
  focusOn = true;
  focusLat = lat; focusLon = lon;
  snprintf(focusName, sizeof(focusName), "%s", name ? name : "");
  followMe = false;
  zoom = 14;                                 // street level, and present on the whole region
  toWorld(lat, lon, zoom, cx, cy);
  dirty = true;
  if (canvasBuf) render();
}
void mapview_clear_focus() { focusOn = false; dirty = true; }

void mapview_centre_on_me() {
  if (!haveOwn) return;
  followMe = true;
  toWorld(ownLat, ownLon, zoom, cx, cy);
  dirty = true;
}
