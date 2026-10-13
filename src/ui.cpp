// The on-device interface: lock screen, a 3x2 home grid, Messages, placeholder apps and a debug
// console. LVGL 8.3 draws into two small strips and flushes through TFT_eSPI's SPI object.
//
// Rules: no blocking calls, no CPU frequency changes, no light sleep. Waking is "any input",
// handled in one place (wake()). Exactly one non-home app screen exists at a time (scrApp); the
// animation that replaces it deletes it (auto_del), so nothing is freed while LVGL still uses it.
#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <time.h>
#include "ui.h"
#include "messages.h"

extern TFT_eSPI tft;

// Austin is UTC-5 in daylight time. This becomes a setting in the Settings app.
static constexpr int32_t TZ_OFFSET_SEC = -5 * 3600;
static constexpr uint32_t IDLE_LOCK_MS = 60000;
static constexpr int PIN_BACKLIGHT = 42;

// ── palette (matches the mockup) ─────────────────────────────────
static constexpr uint32_t INK = 0x0A0E14, RAISE = 0x131A24, RAISE2 = 0x1A2330, LINE = 0x243041;
static constexpr uint32_t TEXT = 0xE8EDF3, MUTE = 0x8593A5, ACCENT = 0xFFB547, GOOD = 0x4ADE80, BAD = 0xFF6B5E;
static constexpr uint32_t BLUE = 0x4C8DFF;
static const uint32_t AVATAR[6] = { 0xFFB547, 0x2FC4B2, 0x8B7CFF, 0xFF7A66, 0x43C77B, 0x4C8DFF };

static inline lv_color_t C(uint32_t h) { return lv_color_hex(h); }

// ── display driver ───────────────────────────────────────────────
static lv_disp_draw_buf_t drawBuf;
static lv_color_t buf1[320 * 24], buf2[320 * 24];
static lv_disp_drv_t dispDrv;
static lv_indev_drv_t touchDrv;

static void flushCb(lv_disp_drv_t* d, const lv_area_t* a, lv_color_t* c) {
  uint32_t w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(a->x1, a->y1, w, h);
  tft.pushColors((uint16_t*)&c->full, w * h, true);
  tft.endWrite();
  lv_disp_flush_ready(d);
}

// ── touch (GT911 over I2C; STOP+START reads, repeated start is unreliable on ESP32) ──
static uint8_t touchAddr = 0;
static bool gt911Read(uint16_t reg, uint8_t* buf, uint8_t n) {
  Wire.beginTransmission(touchAddr);
  Wire.write(reg >> 8); Wire.write(reg & 0xFF);
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom(touchAddr, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}
static void gt911Clear() {
  Wire.beginTransmission(touchAddr);
  Wire.write(0x81); Wire.write(0x4E); Wire.write(0x00);
  Wire.endTransmission(true);
}

// ── state ────────────────────────────────────────────────────────
enum Mode { M_LOCK, M_HOME, M_APP, M_DBG, M_CONVS, M_THREAD };
static Mode mode = M_LOCK;
static uint32_t lastActivityMs = 0;
static bool touchHeld = false;          // a touch that woke the device is ignored until released
static UiSendFn sendFn = nullptr;

static lv_obj_t *scrLock, *scrHome, *scrApp;        // scrApp: whichever non-home screen is open
static lv_obj_t *bar, *barTime, *barSig, *barBatt;
static lv_obj_t *lockTime, *lockDate, *lockMesh;
static lv_obj_t *tiles[6], *badge, *badgeLbl;
static lv_obj_t *dbgLabel, *convBox, *thrBox, *taInput;
static lv_group_t* inputGroup = nullptr;   // gives the message box the focus state that shows its cursor
static int sel = 0;                      // 0..5, the highlighted home tile
static int convSel = 0;                  // highlighted row in the conversation list
static int convOrder[MAX_CONVS], convN = 0;
static int curConv = -1;                 // conversation open in the thread view
static uint32_t shownVersion = 0;
static char dbgCache[720];
static int unreadShown = -1;

struct App { const char* name; const char* sym; uint32_t col; const char* blurb; };
static const App apps[6] = {
  { "Messages", LV_SYMBOL_ENVELOPE, 0x4C8DFF, "" },
  { "Signal", LV_SYMBOL_WIFI, 0xFFB547, "One-tap range test with a log." },
  { "Nodes", LV_SYMBOL_LIST, 0x2FC4B2, "Who you can hear right now." },
  { "Map", LV_SYMBOL_GPS, 0x8B7CFF, "Your position and nearby nodes." },
  { "Status", LV_SYMBOL_BATTERY_3, 0x43C77B, "Battery, uptime, health." },
  { "Settings", LV_SYMBOL_SETTINGS, 0x66758A, "Name, brightness, advanced.\nBuilt next." },
};

// ── helpers ──────────────────────────────────────────────────────
static lv_obj_t* newScreen() {
  lv_obj_t* s = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s, C(INK), 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s, 0, 0);
  lv_obj_set_style_radius(s, 0, 0);
  lv_obj_set_style_pad_all(s, 0, 0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}
// A plain container: transparent unless given a colour. (Without the LVGL theme, objects have
// no background at all, so every visible box has to set bg_opa itself.)
static lv_obj_t* box(lv_obj_t* parent, int w, int h, uint32_t col, int radius) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  if (col != 0xFFFFFFFF) {
    lv_obj_set_style_bg_color(o, C(col), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  } else {
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  }
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}
static lv_obj_t* label(lv_obj_t* parent, const char* txt, const lv_font_t* font, uint32_t col) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, C(col), 0);
  return l;
}
static void setBacklight(int pct) {
  static bool attached = false;
  if (!attached) {
    ledcSetup(0, 5000, 8);
    ledcAttachPin(PIN_BACKLIGHT, 0);
    attached = true;
  }
  ledcWrite(0, (uint32_t)(pct * 255 / 100));
}
static void localTime(uint32_t utc, struct tm& out) {
  time_t t = (time_t)utc + TZ_OFFSET_SEC;
  gmtime_r(&t, &out);
}
static uint32_t avatarColor(const char* name) {
  uint32_t h = 7;
  for (const char* p = name; *p; p++) h = h * 31 + (uint8_t)*p;
  return AVATAR[h % 6];
}

// ── navigation ───────────────────────────────────────────────────
static void showHome();
static void openApp(int i);
static void openConvs();
static void openThread(int conv);
static void goBack();

static void setSel(int i) {
  sel = constrain(i, 0, 5);
  for (int k = 0; k < 6; k++) lv_obj_set_style_outline_width(tiles[k], k == sel ? 3 : 0, 0);
}

static void releaseApp() {            // forget pointers into the app screen before it goes away
  dbgLabel = nullptr; convBox = nullptr; thrBox = nullptr; taInput = nullptr;
  curConv = -1;
  msgs_set_open(-1);
}

static void wake() {
  setBacklight(100);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
  showHome();
}

static void goLock() {
  mode = M_LOCK;
  lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t* old = lv_scr_act();
  lv_scr_load(scrLock);                 // immediate, no animation: the old screen is idle now
  if (old && old == scrApp) { lv_obj_del(old); scrApp = nullptr; }
  releaseApp();
  setBacklight(25);
}

static void showHome() {
  mode = M_HOME;
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t* old = lv_scr_act();
  bool disposable = old && old == scrApp;
  if (disposable) scrApp = nullptr;
  releaseApp();
  if (old == scrHome) lv_scr_load(scrHome);
  else lv_scr_load_anim(scrHome, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, disposable);   // auto_del the app screen
  setSel(sel);
}

// Swap in a new app screen. The animation deletes the screen it replaces, but only if that is
// the one currently shown; Home is never deleted.
static void loadApp(lv_obj_t* s) {
  lv_obj_t* old = scrApp;
  bool del = old && lv_scr_act() == old;
  scrApp = s;
  lv_scr_load_anim(s, LV_SCR_LOAD_ANIM_FADE_ON, 100, 0, del);
}

static void backCb(lv_event_t*) { goBack(); }
static void tileCb(lv_event_t* e) { openApp((int)(intptr_t)lv_event_get_user_data(e)); }

static void goBack() {
  if (mode == M_THREAD) { openConvs(); return; }
  showHome();
}

static lv_obj_t* headerFor(lv_obj_t* scr, const char* title) {
  lv_obj_t* back = box(scr, 40, 28, RAISE, 10);
  lv_obj_set_pos(back, 6, 26);
  lv_obj_t* l = label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_14, TEXT);
  lv_obj_center(l);
  lv_obj_add_event_cb(back, backCb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* t = label(scr, title, &lv_font_montserrat_20, TEXT);
  lv_obj_set_pos(t, 54, 28);
  return back;
}

// ── placeholder apps ─────────────────────────────────────────────
static void openApp(int i) {
  if (i < 0 || i > 5) return;
  if (i == 0) { openConvs(); return; }
  mode = M_APP;
  lv_obj_t* s = newScreen();
  headerFor(s, apps[i].name);
  lv_obj_t* sym = label(s, apps[i].sym, &lv_font_montserrat_48, apps[i].col);
  lv_obj_align(sym, LV_ALIGN_CENTER, 0, -18);
  lv_obj_t* t = label(s, "Coming soon", &lv_font_montserrat_20, TEXT);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, 34);
  lv_obj_t* b = label(s, apps[i].blurb, &lv_font_montserrat_12, MUTE);
  lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(b, LV_ALIGN_CENTER, 0, 70);
  loadApp(s);
}

static void openConsole() {
  mode = M_DBG;
  lv_obj_t* s = newScreen();
  dbgLabel = label(s, "", &lv_font_montserrat_12, TEXT);
  lv_obj_set_pos(dbgLabel, 4, 24);
  lv_obj_set_width(dbgLabel, 312);
  lv_label_set_long_mode(dbgLabel, LV_LABEL_LONG_CLIP);
  dbgCache[0] = 0;
  loadApp(s);
}

// ── Messages: conversation list ──────────────────────────────────
static void convRowCb(lv_event_t* e) { openThread((int)(intptr_t)lv_event_get_user_data(e)); }

static void fillConvs() {
  if (!convBox) return;
  lv_obj_clean(convBox);
  convN = msgs_sorted(convOrder, MAX_CONVS);
  if (convSel >= convN) convSel = max(0, convN - 1);
  if (convN == 0) {
    lv_obj_t* t = label(convBox, "No conversations yet.\nPeople appear when they\nadvertise on the mesh.", &lv_font_montserrat_14, MUTE);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 10);
    return;
  }
  for (int r = 0; r < convN; r++) {
    const Conv* c = msgs_conv(convOrder[r]);
    lv_obj_t* row = box(convBox, 312, 46, r == convSel ? RAISE2 : RAISE, 12);
    lv_obj_set_pos(row, 4, r * 50);
    lv_obj_add_event_cb(row, convRowCb, LV_EVENT_CLICKED, (void*)(intptr_t)convOrder[r]);
    uint32_t col = c->channel ? BLUE : avatarColor(c->name);
    lv_obj_t* av = box(row, 30, 30, col, 15);
    lv_obj_set_pos(av, 8, 8);
    char ini[2] = { c->channel ? '#' : (char)toupper((unsigned char)c->name[0]), 0 };
    lv_obj_t* il = label(av, ini[0] ? ini : "?", &lv_font_montserrat_14, INK);
    lv_obj_center(il);
    lv_obj_t* n = label(row, c->name[0] ? c->name : "(unnamed)", &lv_font_montserrat_14, TEXT);
    lv_obj_set_pos(n, 46, 5);
    lv_obj_set_width(n, 190);
    lv_label_set_long_mode(n, LV_LABEL_LONG_CLIP);
    const Msg* last = c->count ? msgs_at(convOrder[r], c->count - 1) : nullptr;
    lv_obj_t* p = label(row, last ? last->text : "No messages yet", &lv_font_montserrat_12, MUTE);
    lv_obj_set_pos(p, 46, 25);
    lv_obj_set_width(p, 200);
    lv_label_set_long_mode(p, LV_LABEL_LONG_CLIP);
    if (c->unread) {
      lv_obj_t* d = box(row, 22, 22, ACCENT, 11);
      lv_obj_set_pos(d, 282, 12);
      char u[8];
      snprintf(u, sizeof(u), "%d", c->unread > 99 ? 99 : c->unread);
      lv_obj_t* ul = label(d, u, &lv_font_montserrat_12, INK);
      lv_obj_center(ul);
    }
  }
}

static void openConvs() {
  mode = M_CONVS;
  curConv = -1;
  msgs_set_open(-1);
  lv_obj_t* s = newScreen();
  headerFor(s, "Messages");
  convBox = lv_obj_create(s);
  lv_obj_set_size(convBox, 320, 240 - 58);
  lv_obj_set_pos(convBox, 0, 58);
  lv_obj_set_style_bg_opa(convBox, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(convBox, 0, 0);
  lv_obj_set_style_pad_all(convBox, 0, 0);
  lv_obj_set_scrollbar_mode(convBox, LV_SCROLLBAR_MODE_OFF);
  fillConvs();
  shownVersion = msgs_version();
  loadApp(s);
}

// ── Messages: a conversation ─────────────────────────────────────
static void buildThread() {
  if (!thrBox) return;
  lv_obj_clean(thrBox);
  const Conv* c = msgs_conv(curConv);
  if (!c) return;
  for (int i = 0; i < c->count; i++) {
    const Msg* m = msgs_at(curConv, i);
    lv_obj_t* row = lv_obj_create(thrBox);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, m->out ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t* b = lv_obj_create(row);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(b, 236, 0);
    lv_obj_set_style_bg_color(b, C(m->out ? BLUE : RAISE2), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_pad_hor(b, 10, 0);
    lv_obj_set_style_pad_ver(b, 6, 0);
    lv_obj_set_style_pad_row(b, 2, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* t = label(b, m->text, &lv_font_montserrat_14, m->out ? 0x06101F : TEXT);
    lv_obj_set_style_max_width(t, 214, 0);
    if (m->out) {
      const char* st = m->status == MS_DELIVERED ? LV_SYMBOL_OK LV_SYMBOL_OK
                     : m->status == MS_SENT ? LV_SYMBOL_OK
                     : m->status == MS_FAILED ? LV_SYMBOL_WARNING " not delivered"
                     : "sending...";
      lv_obj_t* s = label(b, st, &lv_font_montserrat_12, 0x06101F);
      lv_obj_set_style_align(s, LV_ALIGN_RIGHT_MID, 0);
    }
  }
  lv_obj_update_layout(thrBox);
  lv_obj_scroll_to_y(thrBox, LV_COORD_MAX, LV_ANIM_OFF);
}

static void doSend() {
  if (!taInput || curConv < 0) return;
  const char* t = lv_textarea_get_text(taInput);
  if (!t || !*t) return;
  if (sendFn) sendFn(curConv, t);
  lv_textarea_set_text(taInput, "");
  shownVersion = 0;                      // redraw with the new bubble
}
static void sendCb(lv_event_t*) { doSend(); }

static void openThread(int conv) {
  const Conv* c = msgs_conv(conv);
  if (!c) return;
  mode = M_THREAD;
  curConv = conv;
  msgs_set_open(conv);
  msgs_mark_read(conv);

  lv_obj_t* s = newScreen();
  headerFor(s, c->name[0] ? c->name : "(unnamed)");

  thrBox = lv_obj_create(s);
  lv_obj_set_size(thrBox, 320, 240 - 58 - 60);
  lv_obj_set_pos(thrBox, 0, 58);
  lv_obj_set_style_bg_opa(thrBox, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(thrBox, 0, 0);
  lv_obj_set_style_pad_all(thrBox, 6, 0);
  lv_obj_set_style_pad_row(thrBox, 6, 0);
  lv_obj_set_scrollbar_mode(thrBox, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(thrBox, LV_FLEX_FLOW_COLUMN);

  taInput = lv_textarea_create(s);
  lv_obj_set_size(taInput, 250, 52);                  // two lines of 16 pt text
  lv_obj_set_pos(taInput, 6, 240 - 56);
  lv_textarea_set_one_line(taInput, false);
  lv_textarea_set_max_length(taInput, 140);
  lv_textarea_set_placeholder_text(taInput, "Message");
  lv_obj_set_style_bg_color(taInput, C(RAISE), 0);
  lv_obj_set_style_bg_opa(taInput, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(taInput, C(TEXT), 0);
  lv_obj_set_style_text_font(taInput, &lv_font_montserrat_16, 0);
  lv_obj_set_style_pad_hor(taInput, 10, 0);
  lv_obj_set_style_pad_ver(taInput, 6, 0);
  lv_obj_set_style_border_color(taInput, C(LINE), 0);
  lv_obj_set_style_border_width(taInput, 1, 0);
  lv_obj_set_style_radius(taInput, 14, 0);
  lv_obj_set_scrollbar_mode(taInput, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_text_color(taInput, C(MUTE), LV_PART_TEXTAREA_PLACEHOLDER);
  // Cursor: a blinking amber bar. Without the theme nothing styles this part, so it is invisible.
  lv_obj_set_style_border_side(taInput, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
  lv_obj_set_style_border_width(taInput, 2, LV_PART_CURSOR);
  lv_obj_set_style_border_color(taInput, C(ACCENT), LV_PART_CURSOR);
  lv_obj_set_style_bg_opa(taInput, LV_OPA_TRANSP, LV_PART_CURSOR);
  lv_obj_set_style_anim_time(taInput, 450, LV_PART_CURSOR);
  lv_obj_set_style_border_color(taInput, C(ACCENT), LV_STATE_FOCUSED);   // box outline lights up too
  if (!inputGroup) inputGroup = lv_group_create();
  lv_group_add_obj(inputGroup, taInput);
  lv_group_focus_obj(taInput);

  lv_obj_t* send = box(s, 52, 52, ACCENT, 16);
  lv_obj_set_pos(send, 262, 240 - 56);
  lv_obj_t* sl = label(send, LV_SYMBOL_RIGHT, &lv_font_montserrat_20, INK);
  lv_obj_center(sl);
  lv_obj_add_event_cb(send, sendCb, LV_EVENT_CLICKED, nullptr);

  buildThread();
  shownVersion = msgs_version();
  loadApp(s);
}

// ── building the screens ─────────────────────────────────────────
static void buildStatusBar() {
  bar = box(lv_layer_top(), 320, 22, INK, 0);
  lv_obj_set_pos(bar, 0, 0);
  barTime = label(bar, "--:--", &lv_font_montserrat_14, TEXT);
  lv_obj_align(barTime, LV_ALIGN_LEFT_MID, 8, 0);
  barBatt = label(bar, LV_SYMBOL_BATTERY_3 " --%", &lv_font_montserrat_12, TEXT);
  lv_obj_align(barBatt, LV_ALIGN_RIGHT_MID, -8, 0);
  barSig = label(bar, LV_SYMBOL_WIFI, &lv_font_montserrat_12, MUTE);
  lv_obj_align_to(barSig, barBatt, LV_ALIGN_OUT_LEFT_MID, -10, 0);
}

static void buildLock() {
  scrLock = newScreen();
  lockMesh = label(scrLock, "Starting...", &lv_font_montserrat_12, MUTE);
  lv_obj_align(lockMesh, LV_ALIGN_TOP_MID, 0, 8);
  lockTime = label(scrLock, "--:--", &lv_font_montserrat_48, TEXT);
  lv_obj_align(lockTime, LV_ALIGN_CENTER, 0, -18);
  lockDate = label(scrLock, "", &lv_font_montserrat_14, MUTE);
  lv_obj_align(lockDate, LV_ALIGN_CENTER, 0, 26);
  lv_obj_t* hint = label(scrLock, "Click, touch or press a key", &lv_font_montserrat_12, MUTE);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

static void buildHome() {
  scrHome = newScreen();
  const int colX[3] = { 53, 160, 267 };
  const int rowTop[2] = { 30, 128 };           // tile tops; a label sits just below each tile
  for (int i = 0; i < 6; i++) {
    int cx = colX[i % 3];
    int top = rowTop[i / 3];
    lv_obj_t* t = box(scrHome, 64, 64, apps[i].col, 18);
    lv_obj_set_pos(t, cx - 32, top);
    lv_obj_set_style_outline_color(t, C(TEXT), 0);
    lv_obj_set_style_outline_pad(t, 2, 0);
    lv_obj_set_style_outline_width(t, 0, 0);
    lv_obj_t* s = label(t, apps[i].sym, &lv_font_montserrat_24, INK);
    lv_obj_center(s);
    lv_obj_add_event_cb(t, tileCb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t* n = label(scrHome, apps[i].name, &lv_font_montserrat_14, TEXT);
    lv_obj_update_layout(n);
    lv_obj_set_pos(n, cx - lv_obj_get_width(n) / 2, top + 64 + 6);
    tiles[i] = t;
  }
  // unread badge on the Messages tile
  badge = box(scrHome, 22, 22, BAD, 11);
  lv_obj_set_pos(badge, colX[0] + 32 - 14, rowTop[0] - 8);
  badgeLbl = label(badge, "0", &lv_font_montserrat_12, 0xFFFFFF);
  lv_obj_center(badgeLbl);
  lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
}

// ── touch read callback ──────────────────────────────────────────
static void touchRead(lv_indev_drv_t*, lv_indev_data_t* data) {
  static int16_t lastX = 0, lastY = 0;
  data->state = LV_INDEV_STATE_RELEASED;
  data->point.x = lastX;
  data->point.y = lastY;
  if (!touchAddr) return;
  uint8_t s;
  if (!gt911Read(0x814E, &s, 1)) return;
  if (!(s & 0x80)) return;                       // no new report
  uint8_t n = s & 0x0F;
  bool down = false;
  if (n > 0) {
    uint8_t p[4];
    if (gt911Read(0x8150, p, 4)) {
      int px = p[0] | (p[1] << 8), py = p[2] | (p[3] << 8);
      lastX = (int16_t)constrain(py, 0, 319);       // portrait panel -> landscape (rotation 1)
      lastY = (int16_t)constrain(239 - px, 0, 239);
      down = true;
    }
  }
  gt911Clear();
  if (down) {
    lastActivityMs = millis();
    if (mode == M_LOCK) { wake(); touchHeld = true; }
    if (!touchHeld) data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = lastX;
    data->point.y = lastY;
  } else {
    touchHeld = false;
  }
}

// ── public API ───────────────────────────────────────────────────
void ui_set_send_handler(UiSendFn fn) { sendFn = fn; }

void ui_init() {
  for (uint8_t a : { (uint8_t)0x5D, (uint8_t)0x14 }) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission(true) == 0) { touchAddr = a; break; }
  }

  lv_init();
  lv_disp_draw_buf_init(&drawBuf, buf1, buf2, 320 * 24);
  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res = 320;
  dispDrv.ver_res = 240;
  dispDrv.flush_cb = flushCb;
  dispDrv.draw_buf = &drawBuf;
  lv_disp_drv_register(&dispDrv);

  lv_indev_drv_init(&touchDrv);
  touchDrv.type = LV_INDEV_TYPE_POINTER;
  touchDrv.read_cb = touchRead;
  lv_indev_drv_register(&touchDrv);

  buildStatusBar();
  buildLock();
  buildHome();
  setSel(0);
  lastActivityMs = millis();
  goLock();
}

void ui_activity() { lastActivityMs = millis(); }

void ui_tick() {
  lv_timer_handler();
  if (mode != M_LOCK && millis() - lastActivityMs > IDLE_LOCK_MS) goLock();
}

void ui_update(const UiModel& m) {
  struct tm tm;
  char b[48];
  uint32_t utc = m.clockValid ? m.utc : 0;
  localTime(utc, tm);
  int h12 = tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12;

  if (m.clockValid) snprintf(b, sizeof(b), "%d:%02d", h12, tm.tm_min);
  else snprintf(b, sizeof(b), "--:--");
  lv_label_set_text(barTime, b);
  lv_label_set_text(lockTime, b);

  if (m.clockValid) {
    strftime(b, sizeof(b), "%A, %B %d", &tm);
    lv_label_set_text(lockDate, b);
  } else {
    lv_label_set_text(lockDate, "Waiting for time from GPS or mesh");
  }

  const char* batt = m.charging ? LV_SYMBOL_CHARGE : (m.batteryPct > 75 ? LV_SYMBOL_BATTERY_FULL : m.batteryPct > 50 ? LV_SYMBOL_BATTERY_3 : m.batteryPct > 25 ? LV_SYMBOL_BATTERY_2 : LV_SYMBOL_BATTERY_1);
  snprintf(b, sizeof(b), "%s %d%%", batt, m.batteryPct);
  lv_label_set_text(barBatt, b);
  lv_obj_set_style_text_color(barBatt, C(m.batteryPct <= 15 ? BAD : TEXT), 0);
  lv_obj_set_style_text_color(barSig, C(m.signalBars >= 3 ? GOOD : m.signalBars >= 1 ? ACCENT : MUTE), 0);
  lv_obj_align_to(barSig, barBatt, LV_ALIGN_OUT_LEFT_MID, -10, 0);

  if (m.unread > 0) snprintf(b, sizeof(b), "%d unread  -  %d nodes heard", m.unread, m.nodesHeard);
  else snprintf(b, sizeof(b), m.signalBars > 0 ? "Mesh connected  -  %d nodes heard" : "No mesh heard yet", m.nodesHeard);
  lv_label_set_text(lockMesh, b);

  if (m.unread != unreadShown) {
    unreadShown = m.unread;
    if (m.unread > 0) {
      snprintf(b, sizeof(b), "%d", m.unread > 99 ? 99 : m.unread);
      lv_label_set_text(badgeLbl, b);
      lv_obj_center(badgeLbl);
      lv_obj_clear_flag(badge, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // Redraw Messages screens only when the store actually changed.
  uint32_t v = msgs_version();
  if (v != shownVersion) {
    if (mode == M_CONVS) { fillConvs(); shownVersion = v; }
    else if (mode == M_THREAD) { msgs_mark_read(curConv); buildThread(); shownVersion = msgs_version(); }
  }

  if (mode == M_DBG && dbgLabel && strcmp(dbgCache, m.consoleText) != 0) {
    strncpy(dbgCache, m.consoleText, sizeof(dbgCache) - 1);
    lv_label_set_text(dbgLabel, dbgCache);
  }
}

// Trackball step: moves the highlight on Home and in the conversation list, scrolls a thread.
void ui_nav(int dx, int dy) {
  if (mode == M_LOCK) return;
  ui_activity();
  if (mode == M_HOME) {
    setSel(sel + dx + dy * 3);
  } else if (mode == M_CONVS && dy) {
    convSel = constrain(convSel + dy, 0, max(0, convN - 1));
    fillConvs();
    lv_obj_scroll_to_y(convBox, max(0, convSel * 50 - 100), LV_ANIM_OFF);
  } else if (mode == M_THREAD && dy && thrBox) {
    lv_obj_scroll_by(thrBox, 0, -dy * 40, LV_ANIM_OFF);
  }
}

void ui_click() {
  if (mode == M_LOCK) { ui_activity(); wake(); return; }
  ui_activity();
  if (mode == M_HOME) openApp(sel);
  else if (mode == M_CONVS && convN > 0) openThread(convOrder[convSel]);
  else if (mode == M_THREAD) doSend();
}

void ui_long() {
  ui_activity();
  if (mode == M_LOCK) { wake(); return; }
  showHome();
}

void ui_key(char c) {
  ui_activity();
  if (mode == M_LOCK) { wake(); return; }
  if (mode == M_HOME && (c == 'd' || c == 'D')) { openConsole(); return; }
  if (mode == M_THREAD && taInput) {
    if (c == '\r' || c == '\n') { doSend(); return; }
    if (c == 8 || c == 127) {
      if (lv_textarea_get_text(taInput)[0] == 0) goBack();     // Backspace on an empty box leaves
      else lv_textarea_del_char(taInput);
      return;
    }
    if (c >= 32 && c < 127) lv_textarea_add_char(taInput, (uint32_t)c);
    return;
  }
  if ((mode == M_APP || mode == M_CONVS || mode == M_DBG) && (c == 8 || c == 127)) { goBack(); return; }
}

bool ui_console_active() { return mode == M_DBG; }
