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
#include "settings.h"
#include "nodes.h"
#include "rangetest.h"
#include "fonts/fonts.h"
#include "emoji.h"

extern TFT_eSPI tft;

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
enum Mode { M_LOCK, M_HOME, M_APP, M_DBG, M_CONVS, M_THREAD, M_SETTINGS, M_ENTRY, M_PIN, M_ADV, M_PICK, M_BRIGHT, M_SIGNAL, M_NODES };
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
static lv_obj_t* listBox;                // scrolling list used by Settings and Advanced
static int listN = 0, listSel = 0;
static lv_obj_t *pinMsg, *pinDots[4];
static lv_obj_t *brightSlider, *brightLbl;
static lv_obj_t *nodesBox;
static lv_obj_t *sigTarget, *sigBtnLbl, *sigStatus, *sigVal[4];
static int sigConv = -1;                 // conversation the range test goes to
static uint32_t sigStartMs = 0, sigShownVersion = 0, nodesShownVersion = 0, nodesBuiltMs = 0, nowUtc = 0;
static int sigChoices[MAX_CONVS];
static Mode returnTo = M_SETTINGS;       // where Back from a picker, entry or slider goes
static char pickBuf[24][28];
static int pickN = 0, pickCur = 0;
static void (*pickCb)(int) = nullptr;
static bool (*entryCb)(const char*) = nullptr;
static bool entryNumeric = false;
static char pinBuf[8], pinFirst[8];
static int pinLen = 0, pinStage = 0;     // 0 enter, 1 choose, 2 confirm
static bool resetArmed = false;
static int lastBatt = 0;
static char lastName[32];
static char lastStorage[32];
static UiActions actions = {};
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
  { "Signal", LV_SYMBOL_WIFI, 0xFFB547, "" },
  { "Nodes", LV_SYMBOL_LIST, 0x2FC4B2, "" },
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
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);     // decoration never takes the tap away from its row
  return o;
}
static lv_obj_t* tappable(lv_obj_t* o) {
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}
static lv_obj_t* label(lv_obj_t* parent, const char* txt, const lv_font_t* font, uint32_t col) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, C(col), 0);
  return l;
}
// The T-Deck backlight is an LED driver with a single-wire, pulse-counting interface: each low
// pulse steps the level down (wrapping around), 16 levels. It is not a PWM input, so driving it
// with PWM makes the brightness jump around.
static uint8_t blLevel = 0;              // level the chip is at; 0 = off
static void setBacklight(int level) {
  const int steps = 16;
  level = constrain(level, 0, steps);
  if (level == blLevel) return;
  if (level == 0) {
    digitalWrite(PIN_BACKLIGHT, LOW);
    delay(3);
    blLevel = 0;
    return;
  }
  if (blLevel == 0) {                    // off -> on: the chip comes up at full brightness
    pinMode(PIN_BACKLIGHT, OUTPUT);
    digitalWrite(PIN_BACKLIGHT, HIGH);
    blLevel = steps;
    delayMicroseconds(30);
  }
  int from = steps - blLevel, to = steps - level;
  int pulses = (steps + to - from) % steps;
  for (int i = 0; i < pulses; i++) {
    digitalWrite(PIN_BACKLIGHT, LOW);
    digitalWrite(PIN_BACKLIGHT, HIGH);
  }
  blLevel = level;
}
static void localTime(uint32_t utc, struct tm& out) {
  time_t t = (time_t)utc + ((int32_t)cfg().tzMinutes + (cfg().dst ? 60 : 0)) * 60;
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
static void openNodes();
static void openConvs();
static void openThread(int conv);
static void goBack();

static void setSel(int i) {
  sel = constrain(i, 0, 5);
  for (int k = 0; k < 6; k++) lv_obj_set_style_outline_width(tiles[k], k == sel ? 3 : 0, 0);
}

static void releaseApp() {            // forget pointers into the app screen before it goes away
  dbgLabel = nullptr; convBox = nullptr; thrBox = nullptr; taInput = nullptr;
  listBox = nullptr; listN = 0; pinMsg = nullptr; resetArmed = false;
  brightSlider = nullptr; brightLbl = nullptr; nodesBox = nullptr; sigTarget = nullptr; sigBtnLbl = nullptr; sigStatus = nullptr;
  curConv = -1;
  msgs_set_open(-1);
}

static void wake() {
  setBacklight(cfg().brightness);
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
  setBacklight(min<int>(4, cfg().brightness));      // dimmed: about 25 percent
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

static void openSettings();
static void openAdvanced();
static void openSignal();
static void goBack() {
  if (mode == M_THREAD) { openConvs(); return; }
  if (mode == M_PICK || mode == M_ENTRY || mode == M_BRIGHT) {
    if (returnTo == M_ADV) openAdvanced();
    else if (returnTo == M_SIGNAL) openSignal();
    else openSettings();
    return;
  }
  if (mode == M_PIN || mode == M_ADV) { openSettings(); return; }
  showHome();
}

static lv_obj_t* headerFor(lv_obj_t* scr, const char* title) {
  lv_obj_t* back = tappable(box(scr, 40, 28, RAISE, 10));
  lv_obj_set_pos(back, 6, 26);
  lv_obj_t* l = label(back, LV_SYMBOL_LEFT, &font_tdos_14, TEXT);
  lv_obj_center(l);
  lv_obj_add_event_cb(back, backCb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* t = label(scr, title, &font_tdos_20, TEXT);
  lv_obj_set_pos(t, 54, 28);
  return back;
}

// ── placeholder apps ─────────────────────────────────────────────
static void openApp(int i) {
  if (i < 0 || i > 5) return;
  if (i == 0) { openConvs(); return; }
  if (i == 5) { openSettings(); return; }
  if (i == 1) { openSignal(); return; }
  if (i == 2) { openNodes(); return; }
  mode = M_APP;
  lv_obj_t* s = newScreen();
  headerFor(s, apps[i].name);
  lv_obj_t* sym = label(s, apps[i].sym, &lv_font_montserrat_48, apps[i].col);
  lv_obj_align(sym, LV_ALIGN_CENTER, 0, -18);
  lv_obj_t* t = label(s, "Coming soon", &font_tdos_20, TEXT);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, 34);
  lv_obj_t* b = label(s, apps[i].blurb, &font_tdos_12, MUTE);
  lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(b, LV_ALIGN_CENTER, 0, 70);
  loadApp(s);
}

static void openConsole() {
  mode = M_DBG;
  lv_obj_t* s = newScreen();
  dbgLabel = label(s, "", &font_tdos_12, TEXT);
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
    lv_obj_t* t = label(convBox, "No conversations yet.\nPeople appear when they\nadvertise on the mesh.", &font_tdos_14, MUTE);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 10);
    return;
  }
  for (int r = 0; r < convN; r++) {
    const Conv* c = msgs_conv(convOrder[r]);
    lv_obj_t* row = tappable(box(convBox, 312, 46, r == convSel ? RAISE2 : RAISE, 12));
    lv_obj_set_pos(row, 4, r * 50);
    lv_obj_add_event_cb(row, convRowCb, LV_EVENT_CLICKED, (void*)(intptr_t)convOrder[r]);
    uint32_t col = c->channel ? BLUE : avatarColor(c->name);
    lv_obj_t* av = box(row, 30, 30, col, 15);
    lv_obj_set_pos(av, 8, 8);
    char ini[2] = { c->channel ? '#' : (char)toupper((unsigned char)c->name[0]), 0 };
    lv_obj_t* il = label(av, ini[0] ? ini : "?", &font_tdos_14, INK);
    lv_obj_center(il);
    char nm[48];
    emoji_filter(c->name, nm, sizeof(nm), true);
    lv_obj_t* n = label(row, nm[0] ? nm : "(unnamed)", emoji_font(&font_tdos_14), TEXT);
    lv_obj_set_pos(n, 46, 5);
    lv_obj_set_width(n, 190);
    lv_label_set_long_mode(n, LV_LABEL_LONG_CLIP);
    const Msg* last = c->count ? msgs_at(convOrder[r], c->count - 1) : nullptr;
    char pv[200] = "No messages yet";
    if (last) emoji_filter(last->text, pv, sizeof(pv), true);
    lv_obj_t* p = label(row, pv, emoji_font(&font_tdos_12), MUTE);
    lv_obj_set_pos(p, 46, 25);
    lv_obj_set_width(p, 200);
    lv_label_set_long_mode(p, LV_LABEL_LONG_CLIP);
    if (c->unread) {
      lv_obj_t* d = box(row, 22, 22, ACCENT, 11);
      lv_obj_set_pos(d, 282, 12);
      char u[8];
      snprintf(u, sizeof(u), "%d", c->unread > 99 ? 99 : c->unread);
      lv_obj_t* ul = label(d, u, &font_tdos_12, INK);
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
// "9:22 AM" in the user's time zone, or "" while the clock is not valid.
static void formatClock(uint32_t ts, char* out, size_t n) {
  out[0] = 0;
  if (ts < 1700000000UL) return;
  struct tm tm;
  localTime(ts, tm);
  int h12 = tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12;
  snprintf(out, n, "%d:%02d %s", h12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
}

static void buildThread() {
  if (!thrBox) return;
  lv_obj_clean(thrBox);
  const Conv* c = msgs_conv(curConv);
  if (!c) return;
  for (int i = 0; i < c->count; i++) {
    const Msg* m = msgs_at(curConv, i);

    // one row per message: the bubble, then a small line with the time and delivery state
    lv_obj_t* row = lv_obj_create(thrBox);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_row(row, 2, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    // Items align inside a track that is only as wide as the widest item, and the track itself is
    // then placed in the row: both have to say END for a sent message to sit at the right edge.
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, m->out ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          m->out ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START);

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
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);

    char shown[200];
    emoji_filter(m->text, shown, sizeof(shown), true);
    lv_obj_t* t = label(b, shown, emoji_font(&font_tdos_14), m->out ? 0x06101F : TEXT);
    // A content-sized label never wraps by itself: measure it, and if it is too wide give it a
    // fixed width so LVGL wraps it onto more lines.
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_update_layout(t);
    if (lv_obj_get_width(t) > 214) lv_obj_set_width(t, 214);

    char when[16], meta[64];
    formatClock(m->ts, when, sizeof(when));
    uint32_t metaCol = MUTE;
    if (!m->out) {
      snprintf(meta, sizeof(meta), "%s", when);
    } else if (m->status == MS_DELIVERED) {
      char rtt[16] = "";
      if (m->rtt100) snprintf(rtt, sizeof(rtt), " %.1fs", m->rtt100 / 10.0f);
      snprintf(meta, sizeof(meta), "%s  " LV_SYMBOL_OK LV_SYMBOL_OK "%s", when, rtt);
      metaCol = GOOD;
    } else if (m->status == MS_SENT) {
      snprintf(meta, sizeof(meta), "%s  " LV_SYMBOL_OK, when);
    } else if (m->status == MS_FAILED) {
      snprintf(meta, sizeof(meta), "%s  " LV_SYMBOL_WARNING " not delivered", when);
      metaCol = BAD;
    } else {
      snprintf(meta, sizeof(meta), "%s  sending...", when);
    }
    if (meta[0]) label(row, meta, &font_tdos_12, metaCol);
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
  char title[48];
  emoji_filter(c->name, title, sizeof(title), false);
  headerFor(s, title[0] ? title : "(unnamed)");

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
  lv_obj_set_style_text_font(taInput, &font_tdos_16, 0);
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

  lv_obj_t* send = tappable(box(s, 52, 52, ACCENT, 16));
  lv_obj_set_pos(send, 262, 240 - 56);
  lv_obj_t* sl = label(send, LV_SYMBOL_RIGHT, &font_tdos_20, INK);
  lv_obj_center(sl);
  lv_obj_add_event_cb(send, sendCb, LV_EVENT_CLICKED, nullptr);

  buildThread();
  shownVersion = msgs_version();
  loadApp(s);
}


// ── Toast: a short confirmation at the bottom of the screen ───────
static lv_obj_t* toastObj = nullptr;
static uint32_t toastGen = 0;

static void toastDone(lv_timer_t* t) {
  if ((uint32_t)(uintptr_t)t->user_data == toastGen && toastObj) {   // ignore timers of superseded toasts
    lv_obj_del(toastObj);
    toastObj = nullptr;
  }
}

static void toast(const char* text) {
  if (toastObj) { lv_obj_del(toastObj); toastObj = nullptr; }
  toastObj = box(lv_layer_top(), 10, 30, RAISE2, 15);
  lv_obj_set_style_border_width(toastObj, 1, 0);
  lv_obj_set_style_border_color(toastObj, C(ACCENT), 0);
  lv_obj_t* l = label(toastObj, text, &font_tdos_14, TEXT);
  lv_obj_center(l);
  lv_obj_update_layout(l);
  lv_obj_set_width(toastObj, lv_obj_get_width(l) + 32);
  lv_obj_align(toastObj, LV_ALIGN_BOTTOM_MID, 0, -14);
  lv_obj_move_foreground(toastObj);
  toastGen++;
  lv_timer_t* t = lv_timer_create(toastDone, 1800, (void*)(uintptr_t)toastGen);
  lv_timer_set_repeat_count(t, 1);
}

static void toast(const char* text);

// ── Settings ─────────────────────────────────────────────────────
struct Preset { const char* name; float freq, bw; uint8_t sf, cr; };
static const Preset presets[] = {
  { "US / Canada", 910.525f, 62.5f, 7, 5 },
  { "EU / UK 868", 869.618f, 62.5f, 8, 5 },
  { "Default 915", 915.0f, 250.0f, 10, 5 },
};
static constexpr int PRESET_N = 3;

struct Zone { const char* name; int16_t minutes; };
static const Zone zones[] = {
  { "Hawaii", -600 }, { "Alaska", -540 }, { "Pacific", -480 }, { "Mountain", -420 }, { "Central", -360 },
  { "Eastern", -300 }, { "Atlantic", -240 }, { "Newfoundland", -210 }, { "Brasilia", -180 },
  { "UTC / Greenwich", 0 }, { "Central Europe", 60 }, { "Eastern Europe", 120 }, { "Moscow", 180 },
  { "Gulf", 240 }, { "Pakistan", 300 }, { "India", 330 }, { "Bangladesh", 360 }, { "Thailand", 420 },
  { "China / Singapore", 480 }, { "Japan / Korea", 540 }, { "Australia East", 600 }, { "New Zealand", 720 },
};
static constexpr int ZONE_N = sizeof(zones) / sizeof(zones[0]);
static const float bwList[] = { 7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f };
static constexpr int BW_N = 10;

enum { ROW_NAME, ROW_TZ, ROW_DST, ROW_BRIGHT, ROW_RADIO, ROW_BATT, ROW_ABOUT, ROW_ADV };
enum { AD_PRESET, AD_FREQ, AD_BW, AD_SF, AD_CR, AD_PWR, AD_ADVERT, AD_PIN, AD_RESET, AD_INFO };

void ui_set_actions(const UiActions& a) { actions = a; }

// Row kinds, so what you can do is visible at a glance:
//   K_NAV    opens something (a list, a slider, a text box)   chevron on the right
//   K_VALUE  a two-state setting you flip in place             amber chip
//   K_BUTTON does something now                                amber pill
//   K_INFO   read only                                         flat and dim, never highlighted
enum RowKind { K_NAV, K_VALUE, K_BUTTON, K_INFO };
static int listKind[24];
static lv_obj_t* listRows[24];
static int listIds[24];

static void listHighlight() {
  for (int i = 0; i < listN; i++) {
    if (listKind[i] == K_INFO) continue;
    lv_obj_set_style_bg_color(listRows[i], C(i == listSel ? RAISE2 : RAISE), 0);
    lv_obj_set_style_border_width(listRows[i], i == listSel ? 2 : 0, 0);
    lv_obj_set_style_border_color(listRows[i], C(ACCENT), 0);
  }
}
static int nextSelectable(int from, int dir) {
  for (int i = from + dir; i >= 0 && i < listN; i += dir)
    if (listKind[i] != K_INFO) return i;
  return from;
}

static void newListBox(lv_obj_t* scr) {
  listBox = lv_obj_create(scr);
  lv_obj_set_size(listBox, 320, 240 - 58);
  lv_obj_set_pos(listBox, 0, 58);
  lv_obj_set_style_bg_opa(listBox, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(listBox, 0, 0);
  lv_obj_set_style_pad_all(listBox, 0, 0);
  lv_obj_set_scrollbar_mode(listBox, LV_SCROLLBAR_MODE_OFF);
  listN = 0;
}

static void settingsAct(int id);
static void advAct(int id);
static void pickChoose(int idx);

static void listRowCb(lv_event_t* e) {
  int id = (int)(intptr_t)lv_event_get_user_data(e);
  for (int i = 0; i < listN; i++) if (listIds[i] == id) listSel = i;
  if (mode == M_SETTINGS) settingsAct(id);
  else if (mode == M_ADV) advAct(id);
  else if (mode == M_PICK) pickChoose(id);
}

static lv_obj_t* addRow(const char* title, const char* value, int id, RowKind kind, bool on = false) {
  if (listN >= 24) return nullptr;
  bool info = kind == K_INFO;
  lv_obj_t* r = box(listBox, 312, 34, info ? 0xFFFFFFFF : RAISE, 10);
  lv_obj_set_pos(r, 4, listN * 38);
  lv_obj_t* t = label(r, title, &font_tdos_14, info ? MUTE : TEXT);
  lv_obj_align(t, LV_ALIGN_LEFT_MID, 10, 0);

  if (kind == K_NAV) {
    if (value && *value) {
      lv_obj_t* v = label(r, value, &font_tdos_14, ACCENT);
      lv_obj_align(v, LV_ALIGN_RIGHT_MID, -28, 0);
    }
    lv_obj_t* c = label(r, LV_SYMBOL_RIGHT, &font_tdos_14, TEXT);
    lv_obj_align(c, LV_ALIGN_RIGHT_MID, -10, 0);
  } else if (kind == K_VALUE) {
    lv_obj_t* chip = box(r, 10, 24, on ? ACCENT : RAISE2, 12);
    lv_obj_t* v = label(chip, value, &font_tdos_14, on ? INK : MUTE);
    lv_obj_center(v);
    lv_obj_update_layout(v);
    lv_obj_set_width(chip, lv_obj_get_width(v) + 24);
    lv_obj_align(chip, LV_ALIGN_RIGHT_MID, -6, 0);
  } else if (kind == K_BUTTON) {
    lv_obj_t* pill = box(r, 10, 24, ACCENT, 12);
    lv_obj_t* v = label(pill, value && *value ? value : "Go", &font_tdos_14, INK);
    lv_obj_center(v);
    lv_obj_update_layout(v);
    lv_obj_set_width(pill, lv_obj_get_width(v) + 24);
    lv_obj_align(pill, LV_ALIGN_RIGHT_MID, -6, 0);
  } else if (info && value && *value) {
    lv_obj_t* v = label(r, value, &font_tdos_14, TEXT);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -10, 0);
  }

  if (!info) {
    tappable(r);
    lv_obj_add_event_cb(r, listRowCb, LV_EVENT_CLICKED, (void*)(intptr_t)id);
  }
  listRows[listN] = r;
  listIds[listN] = id;
  listKind[listN] = kind;
  listN++;
  return r;
}

static void fmtOffset(int minutes, char* out, size_t n) {
  int a = abs(minutes);
  if (a % 60) snprintf(out, n, "UTC%c%d:%02d", minutes < 0 ? '-' : '+', a / 60, a % 60);
  else snprintf(out, n, "UTC%c%d", minutes < 0 ? '-' : '+', a / 60);
}
static int zoneIndex() {
  for (int i = 0; i < ZONE_N; i++) if (zones[i].minutes == cfg().tzMinutes) return i;
  return -1;
}
static int presetIndex() {
  const Settings& c = cfg();
  for (int i = 0; i < PRESET_N; i++)
    if (fabsf(presets[i].freq - c.freq) < 0.0005f && presets[i].bw == c.bw && presets[i].sf == c.sf && presets[i].cr == c.cr) return i;
  return -1;
}

// ── Picker: a scrollable list to choose one value from ───────────
// The caller fills pickBuf[0..n), then calls openPicker. Choosing an entry runs cb(index), which
// applies the value and makes exactly one screen change back.
static void openPicker(const char* title, int n, int cur, void (*cb)(int), Mode ret) {
  mode = M_PICK;
  returnTo = ret;
  pickN = n; pickCur = cur; pickCb = cb;
  lv_obj_t* s = newScreen();
  headerFor(s, title);
  newListBox(s);
  for (int i = 0; i < n; i++) {
    lv_obj_t* r = addRow(pickBuf[i], "", i, K_NAV);
    if (!r) break;
    // replace the chevron with a check on the current choice
    lv_obj_t* last = lv_obj_get_child(r, lv_obj_get_child_cnt(r) - 1);
    if (i == cur) lv_label_set_text(last, LV_SYMBOL_OK);
    else lv_label_set_text(last, "");
  }
  listSel = constrain(cur, 0, max(0, listN - 1));
  listHighlight();
  lv_obj_scroll_to_y(listBox, max(0, listSel * 38 - 76), LV_ANIM_OFF);
  loadApp(s);
}
static void pickChoose(int idx) {
  if (pickCb) pickCb(idx);
}

// ── Entry: a text box with Save and Cancel ───────────────────────
static void entrySave() {
  if (!taInput || !entryCb) return;
  if (entryCb(lv_textarea_get_text(taInput))) { /* the callback navigates */ }
}
static void entrySaveCb(lv_event_t*) { entrySave(); }
static void entryCancelCb(lv_event_t*) { goBack(); }

static void openEntry(const char* title, const char* hint, const char* initial, int maxLen, bool numeric,
                      bool (*cb)(const char*), Mode ret) {
  mode = M_ENTRY;
  returnTo = ret;
  entryCb = cb;
  entryNumeric = numeric;
  lv_obj_t* s = newScreen();
  headerFor(s, title);
  lv_obj_t* h = label(s, hint, &font_tdos_12, MUTE);
  lv_obj_set_pos(h, 12, 66);
  lv_obj_set_width(h, 296);
  taInput = lv_textarea_create(s);
  lv_obj_set_size(taInput, 296, 44);
  lv_obj_set_pos(taInput, 12, 100);
  lv_textarea_set_one_line(taInput, true);
  lv_textarea_set_max_length(taInput, maxLen);
  lv_textarea_set_text(taInput, initial);
  lv_obj_set_style_bg_color(taInput, C(RAISE), 0);
  lv_obj_set_style_bg_opa(taInput, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(taInput, C(TEXT), 0);
  lv_obj_set_style_text_font(taInput, &font_tdos_20, 0);
  lv_obj_set_style_pad_hor(taInput, 12, 0);
  lv_obj_set_style_border_color(taInput, C(ACCENT), 0);
  lv_obj_set_style_border_width(taInput, 2, 0);
  lv_obj_set_style_radius(taInput, 14, 0);
  lv_obj_set_style_border_side(taInput, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
  lv_obj_set_style_border_width(taInput, 2, LV_PART_CURSOR);
  lv_obj_set_style_border_color(taInput, C(ACCENT), LV_PART_CURSOR);
  lv_obj_set_style_bg_opa(taInput, LV_OPA_TRANSP, LV_PART_CURSOR);
  lv_obj_set_style_anim_time(taInput, 450, LV_PART_CURSOR);
  if (!inputGroup) inputGroup = lv_group_create();
  lv_group_add_obj(inputGroup, taInput);
  lv_group_focus_obj(taInput);

  lv_obj_t* cancel = tappable(box(s, 140, 44, RAISE2, 14));
  lv_obj_set_pos(cancel, 12, 160);
  lv_obj_center(label(cancel, "Cancel", &font_tdos_16, TEXT));
  lv_obj_add_event_cb(cancel, entryCancelCb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* save = tappable(box(s, 140, 44, ACCENT, 14));
  lv_obj_set_pos(save, 168, 160);
  lv_obj_center(label(save, "Save", &font_tdos_16, INK));
  lv_obj_add_event_cb(save, entrySaveCb, LV_EVENT_CLICKED, nullptr);
  loadApp(s);
}

static bool saveName(const char* t) {
  char clean[32];
  strncpy(clean, t, sizeof(clean) - 1);
  clean[sizeof(clean) - 1] = 0;
  int n = strlen(clean);
  while (n > 0 && clean[n - 1] == ' ') clean[--n] = 0;
  if (n == 0) { toast("A name cannot be empty"); return false; }
  strncpy(cfg().name, clean, sizeof(cfg().name) - 1);
  settings_changed();
  if (actions.setName) actions.setName(clean);
  strncpy(lastName, clean, sizeof(lastName) - 1);
  openSettings();
  toast("Name saved, advert sent");
  return true;
}

static bool saveFreq(const char* t) {
  float f = atof(t);
  if (f < 400.0f || f > 960.0f) { toast("Use 400 to 960 MHz"); return false; }
  cfg().freq = roundf(f * 1000.0f) / 1000.0f;
  settings_changed();
  if (actions.applyRadio) actions.applyRadio();
  openAdvanced();
  toast("Frequency set");
  return true;
}

// ── Brightness: a slider over the chip's 16 levels ───────────────
static void brightApply(int level) {
  level = constrain(level, 1, 16);
  cfg().brightness = (uint8_t)level;
  setBacklight(level);
  settings_changed();
  if (brightLbl) {
    char b[16];
    snprintf(b, sizeof(b), "%d%%", level * 100 / 16);
    lv_label_set_text(brightLbl, b);
  }
}
static void brightCb(lv_event_t* e) {
  brightApply(lv_slider_get_value(lv_event_get_target(e)));
}

static void openBrightness() {
  mode = M_BRIGHT;
  returnTo = M_SETTINGS;
  lv_obj_t* s = newScreen();
  headerFor(s, "Brightness");
  char b[16];
  snprintf(b, sizeof(b), "%d%%", cfg().brightness * 100 / 16);
  brightLbl = label(s, b, &lv_font_montserrat_48, TEXT);
  lv_obj_align(brightLbl, LV_ALIGN_CENTER, 0, -20);
  brightSlider = lv_slider_create(s);
  lv_obj_set_size(brightSlider, 270, 18);
  lv_obj_align(brightSlider, LV_ALIGN_CENTER, 0, 44);
  lv_slider_set_range(brightSlider, 1, 16);
  lv_slider_set_value(brightSlider, cfg().brightness, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(brightSlider, C(RAISE2), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(brightSlider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(brightSlider, C(ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(brightSlider, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(brightSlider, C(TEXT), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(brightSlider, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_pad_all(brightSlider, 6, LV_PART_KNOB);
  lv_obj_add_event_cb(brightSlider, brightCb, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_t* hint = label(s, "Drag the bar, or roll the trackball sideways", &font_tdos_12, MUTE);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);
  loadApp(s);
}

// ── Pickers' value callbacks ─────────────────────────────────────
static void pickZone(int i) {
  cfg().tzMinutes = zones[constrain(i, 0, ZONE_N - 1)].minutes;
  settings_changed();
  openSettings();
}
static void pickPreset(int i) {
  const Preset& p = presets[constrain(i, 0, PRESET_N - 1)];
  Settings& c = cfg();
  c.freq = p.freq; c.bw = p.bw; c.sf = p.sf; c.cr = p.cr;
  settings_changed();
  if (actions.applyRadio) actions.applyRadio();
  openAdvanced();
}
static void pickBw(int i) { cfg().bw = bwList[constrain(i, 0, BW_N - 1)]; settings_changed(); if (actions.applyRadio) actions.applyRadio(); openAdvanced(); }
static void pickSf(int i) { cfg().sf = 5 + constrain(i, 0, 7); settings_changed(); if (actions.applyRadio) actions.applyRadio(); openAdvanced(); }
static void pickCr(int i) { cfg().cr = 5 + constrain(i, 0, 3); settings_changed(); if (actions.applyRadio) actions.applyRadio(); openAdvanced(); }
static void pickPwr(int i) { cfg().txPower = 2 + constrain(i, 0, 20); settings_changed(); if (actions.applyRadio) actions.applyRadio(); openAdvanced(); }

// ── Settings screen ──────────────────────────────────────────────
static void fillSettings() {
  if (!listBox) return;
  lv_obj_clean(listBox);
  listN = 0;
  Settings& c = cfg();
  char b[48], off[16];
  addRow("Name", lastName, ROW_NAME, K_NAV);
  int z = zoneIndex();
  fmtOffset(c.tzMinutes, off, sizeof(off));
  if (z >= 0) snprintf(b, sizeof(b), "%s", zones[z].name);
  else snprintf(b, sizeof(b), "%s", off);
  addRow("Time zone", b, ROW_TZ, K_NAV);
  addRow("Daylight saving", c.dst ? "On" : "Off", ROW_DST, K_VALUE, c.dst);
  snprintf(b, sizeof(b), "%d%%", c.brightness * 100 / 16);
  addRow("Brightness", b, ROW_BRIGHT, K_NAV);
  int p = presetIndex();
  addRow("Radio", p >= 0 ? presets[p].name : "Custom", ROW_RADIO, K_INFO);
  snprintf(b, sizeof(b), "%d%%", lastBatt);
  addRow("Battery", b, ROW_BATT, K_INFO);
  addRow("History", lastStorage, ROW_ABOUT, K_INFO);
  addRow("About", "tdeck-os 0.1", ROW_ABOUT, K_INFO);
  addRow("Advanced", "", ROW_ADV, K_NAV);
  if (listKind[listSel] == K_INFO) listSel = nextSelectable(listSel, 1);
  listHighlight();
}

static void openSettings() {
  mode = M_SETTINGS;
  lv_obj_t* s = newScreen();
  headerFor(s, "Settings");
  newListBox(s);
  listSel = 0;
  fillSettings();
  loadApp(s);
}

// ── PIN entry ────────────────────────────────────────────────────
void ui_pin_key(int k);
void ui_pin_refresh();

// First run: the owner chooses a PIN. Afterwards it is entered to get in. One screen transition
// only: callers must not open another screen from the same tap that leads here.
static void openPin() {
  mode = M_PIN;
  pinLen = 0;
  pinBuf[0] = 0;
  pinStage = cfg().pinSet ? 0 : 1;
  lv_obj_t* s = newScreen();
  headerFor(s, "Advanced");
  pinMsg = label(s, "", &font_tdos_14, TEXT);
  lv_obj_set_pos(pinMsg, 12, 70);
  lv_obj_set_width(pinMsg, 150);
  for (int i = 0; i < 4; i++) {
    pinDots[i] = box(s, 16, 16, LINE, 8);
    lv_obj_set_pos(pinDots[i], 14 + i * 28, 150);
  }
  const char* keys[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK };
  for (int k = 0; k < 12; k++) {
    lv_obj_t* kb = tappable(box(s, 46, 36, RAISE, 10));
    lv_obj_set_pos(kb, 168 + (k % 3) * 50, 60 + (k / 3) * 40);
    lv_obj_center(label(kb, keys[k], &font_tdos_20, TEXT));
    lv_obj_add_event_cb(kb, [](lv_event_t* e) { ui_pin_key((int)(intptr_t)lv_event_get_user_data(e)); },
                        LV_EVENT_CLICKED, (void*)(intptr_t)k);
  }
  loadApp(s);
  ui_pin_refresh();
}

static void openAdvanced();

void ui_pin_refresh() {
  if (!pinMsg) return;
  static const char* const prompts[] = { "Enter PIN", "Choose a 4-digit PIN for Advanced settings", "Enter it again to confirm" };
  lv_label_set_text(pinMsg, prompts[pinStage]);
  for (int i = 0; i < 4; i++) lv_obj_set_style_bg_color(pinDots[i], C(i < pinLen ? ACCENT : LINE), 0);
}

// k: 0..8 = digits 1..9, 9 = backspace, 10 = digit 0, 11 = enter (the PIN submits itself at 4)
void ui_pin_key(int k) {
  ui_activity();
  if (k == 9) { if (pinLen > 0) pinBuf[--pinLen] = 0; ui_pin_refresh(); return; }
  if (k == 11) return;
  char d = (k == 10) ? '0' : (char)('1' + k);
  if (pinLen >= 4) return;
  pinBuf[pinLen++] = d;
  pinBuf[pinLen] = 0;
  ui_pin_refresh();
  if (pinLen < 4) return;

  Settings& c = cfg();
  bool wrong = false;
  if (pinStage == 0) {
    if (strcmp(pinBuf, c.pin) == 0) { openAdvanced(); return; }
    wrong = true;
  } else if (pinStage == 1) {
    strcpy(pinFirst, pinBuf);
    pinStage = 2;
  } else {
    if (strcmp(pinBuf, pinFirst) == 0) {
      strcpy(c.pin, pinBuf);
      c.pinSet = 1;
      settings_changed();
      openAdvanced();
      toast("PIN saved");
      return;
    }
    pinStage = 1;
    pinLen = 0; pinBuf[0] = 0;
    ui_pin_refresh();
    lv_label_set_text(pinMsg, "Those did not match. Choose a PIN again.");
    return;
  }
  pinLen = 0; pinBuf[0] = 0;
  ui_pin_refresh();
  if (wrong) lv_label_set_text(pinMsg, "Wrong PIN. Try again.");
}

// ── Advanced ─────────────────────────────────────────────────────
static void fillAdvanced() {
  if (!listBox) return;
  int keepScroll = lv_obj_get_scroll_y(listBox);
  lv_obj_clean(listBox);
  listN = 0;
  Settings& c = cfg();
  char b[48];
  int p = presetIndex();
  addRow("Radio preset", p >= 0 ? presets[p].name : "Custom", AD_PRESET, K_NAV);
  snprintf(b, sizeof(b), "%.3f MHz", c.freq);
  addRow("Frequency", b, AD_FREQ, K_NAV);
  snprintf(b, sizeof(b), "%.4g kHz", c.bw);
  addRow("Bandwidth", b, AD_BW, K_NAV);
  snprintf(b, sizeof(b), "SF %d", c.sf);
  addRow("Spreading factor", b, AD_SF, K_NAV);
  snprintf(b, sizeof(b), "4/%d", c.cr);
  addRow("Coding rate", b, AD_CR, K_NAV);
  snprintf(b, sizeof(b), "%d dBm", c.txPower);
  addRow("TX power", b, AD_PWR, K_NAV);
  addRow("Send advert", "Send", AD_ADVERT, K_BUTTON);
  addRow("PIN", "Change", AD_PIN, K_BUTTON);
  addRow(resetArmed ? "Tap again to confirm" : "Restore defaults", resetArmed ? "Confirm" : "Restore", AD_RESET, K_BUTTON);
  if (resetArmed) lv_obj_set_style_bg_color(listRows[listN - 1], C(0x4A1F1A), 0);
  addRow("Build", "MeshCore 1.x", AD_INFO, K_INFO);
  if (listSel >= listN) listSel = listN - 1;
  if (listKind[listSel] == K_INFO) listSel = nextSelectable(listSel, -1);
  listHighlight();
  lv_obj_scroll_to_y(listBox, keepScroll, LV_ANIM_OFF);
}

static void openAdvanced() {
  mode = M_ADV;
  pinMsg = nullptr;
  lv_obj_t* s = newScreen();
  headerFor(s, "Advanced");
  newListBox(s);
  listSel = 0;
  resetArmed = false;
  fillAdvanced();
  loadApp(s);
}

static void settingsAct(int id) {
  Settings& c = cfg();
  switch (id) {
    case ROW_NAME:
      openEntry("Name", "How other people see you on the mesh.", lastName, 31, false, saveName, M_SETTINGS);
      return;
    case ROW_TZ: {
      char off[16];
      for (int i = 0; i < ZONE_N; i++) {
        fmtOffset(zones[i].minutes, off, sizeof(off));
        snprintf(pickBuf[i], sizeof(pickBuf[i]), "%s  %s", zones[i].name, off);
      }
      openPicker("Time zone", ZONE_N, zoneIndex(), pickZone, M_SETTINGS);
      return;
    }
    case ROW_DST:
      c.dst = c.dst ? 0 : 1;
      break;
    case ROW_BRIGHT: openBrightness(); return;
    case ROW_ADV: openPin(); return;
    default: return;                                          // read-only rows
  }
  settings_changed();
  fillSettings();
}

static void advAct(int id) {
  Settings& c = cfg();
  switch (id) {
    case AD_PRESET:
      for (int i = 0; i < PRESET_N; i++) snprintf(pickBuf[i], sizeof(pickBuf[i]), "%s", presets[i].name);
      openPicker("Radio preset", PRESET_N, presetIndex(), pickPreset, M_ADV);
      return;
    case AD_FREQ: {
      char f[16];
      snprintf(f, sizeof(f), "%.3f", c.freq);
      openEntry("Frequency", "In MHz, from 400 to 960.", f, 8, true, saveFreq, M_ADV);
      return;
    }
    case AD_BW: {
      int cur = 0;
      for (int i = 0; i < BW_N; i++) {
        snprintf(pickBuf[i], sizeof(pickBuf[i]), "%.4g kHz", bwList[i]);
        if (fabsf(bwList[i] - c.bw) < fabsf(bwList[cur] - c.bw)) cur = i;
      }
      openPicker("Bandwidth", BW_N, cur, pickBw, M_ADV);
      return;
    }
    case AD_SF:
      for (int i = 0; i < 8; i++) snprintf(pickBuf[i], sizeof(pickBuf[i]), "SF %d", 5 + i);
      openPicker("Spreading factor", 8, constrain(c.sf - 5, 0, 7), pickSf, M_ADV);
      return;
    case AD_CR:
      for (int i = 0; i < 4; i++) snprintf(pickBuf[i], sizeof(pickBuf[i]), "4/%d", 5 + i);
      openPicker("Coding rate", 4, constrain(c.cr - 5, 0, 3), pickCr, M_ADV);
      return;
    case AD_PWR:
      for (int i = 0; i < 21; i++) snprintf(pickBuf[i], sizeof(pickBuf[i]), "%d dBm", 2 + i);
      openPicker("TX power", 21, constrain(c.txPower - 2, 0, 20), pickPwr, M_ADV);
      return;
    case AD_ADVERT:
      if (actions.advertise) actions.advertise();
      toast("Advert sent");
      return;
    case AD_PIN:
      c.pinSet = 0;
      settings_changed();
      openPin();                         // straight into "choose a PIN", one transition
      return;
    case AD_RESET:
      if (!resetArmed) { resetArmed = true; fillAdvanced(); return; }
      settings_defaults(true);
      if (actions.applyRadio) actions.applyRadio();
      resetArmed = false;
      openSettings();
      toast("Defaults restored");
      return;
    default: return;
  }
}

// ── Nodes: everyone heard on the mesh ────────────────────────────
static const char* typeName(uint8_t t) {
  return t == 1 ? "Person" : t == 2 ? "Repeater" : t == 3 ? "Room" : t == 4 ? "Sensor" : "Node";
}
static uint32_t typeColor(uint8_t t, const char* name) {
  return t == 2 ? 0x2FC4B2 : t == 3 ? 0x8B7CFF : t == 4 ? 0x43C77B : avatarColor(name);
}
static void formatAge(uint32_t ts, char* out, size_t n) {
  if (!ts || nowUtc < 1700000000UL || nowUtc < ts) { snprintf(out, n, "-"); return; }
  uint32_t d = nowUtc - ts;
  if (d < 60) snprintf(out, n, "now");
  else if (d < 3600) snprintf(out, n, "%um", (unsigned)(d / 60));
  else if (d < 86400) snprintf(out, n, "%uh", (unsigned)(d / 3600));
  else snprintf(out, n, "%ud", (unsigned)(d / 86400));
}

static void fillNodes() {
  if (!nodesBox) return;
  int keep = lv_obj_get_scroll_y(nodesBox);
  lv_obj_clean(nodesBox);
  int n = nodes_count();
  if (n == 0) {
    lv_obj_t* t = label(nodesBox, "Nobody heard yet.\\nNodes appear as they advertise.", &font_tdos_14, MUTE);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 10);
    return;
  }
  for (int r = 0; r < n; r++) {
    const NodeInfo* ni = nodes_sorted(r);
    lv_obj_t* row = box(nodesBox, 312, 46, RAISE, 12);
    lv_obj_set_pos(row, 4, r * 50);
    lv_obj_t* av = box(row, 30, 30, typeColor(ni->type, ni->name), 15);
    lv_obj_set_pos(av, 8, 8);
    char ini[2] = { ni->type == 2 ? 'R' : ni->type == 3 ? 'S' : (char)toupper((unsigned char)ni->name[0]), 0 };
    lv_obj_center(label(av, ini[0] ? ini : "?", &font_tdos_14, INK));

    char nm[48];
    emoji_filter(ni->name, nm, sizeof(nm), true);
    lv_obj_t* name = label(row, nm[0] ? nm : "(unnamed)", emoji_font(&font_tdos_14), TEXT);
    lv_obj_set_pos(name, 46, 4);
    lv_obj_set_width(name, 180);
    lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);

    char sub[48];
    if (ni->hops < 0) snprintf(sub, sizeof(sub), "%s", typeName(ni->type));
    else if (ni->hops == 0) snprintf(sub, sizeof(sub), "%s, direct", typeName(ni->type));
    else snprintf(sub, sizeof(sub), "%s, %d hop%s", typeName(ni->type), ni->hops, ni->hops == 1 ? "" : "s");
    lv_obj_t* sl = label(row, sub, &font_tdos_12, MUTE);
    lv_obj_set_pos(sl, 46, 25);

    char sig[24], age[12];
    formatAge(ni->lastTs, age, sizeof(age));
    if (ni->rssi) snprintf(sig, sizeof(sig), "%d dBm\\n%s", ni->rssi, age);
    else snprintf(sig, sizeof(sig), "\\n%s", age);
    lv_obj_t* rt = label(row, sig, &font_tdos_12, ni->rssi > -100 || !ni->rssi ? TEXT : ACCENT);
    lv_obj_set_style_text_align(rt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(rt, LV_ALIGN_RIGHT_MID, -10, 0);
  }
  lv_obj_scroll_to_y(nodesBox, keep, LV_ANIM_OFF);
}

static void openNodes() {
  mode = M_NODES;
  lv_obj_t* s = newScreen();
  headerFor(s, "Nodes");
  nodesBox = lv_obj_create(s);
  lv_obj_set_size(nodesBox, 320, 240 - 58);
  lv_obj_set_pos(nodesBox, 0, 58);
  lv_obj_set_style_bg_opa(nodesBox, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(nodesBox, 0, 0);
  lv_obj_set_style_pad_all(nodesBox, 0, 0);
  lv_obj_set_scrollbar_mode(nodesBox, LV_SCROLLBAR_MODE_OFF);
  fillNodes();
  nodesShownVersion = nodes_version();
  nodesBuiltMs = millis();
  loadApp(s);
}

// ── Signal: a one-tap range test ─────────────────────────────────
static void sigPickDefault() {
  const Conv* c = msgs_conv(sigConv);
  if (c && !c->channel) return;
  int order[MAX_CONVS];
  int n = msgs_sorted(order, MAX_CONVS);
  sigConv = -1;
  for (int i = 0; i < n; i++) {
    const Conv* k = msgs_conv(order[i]);
    if (k && !k->channel) { sigConv = order[i]; break; }
  }
}

static void refreshSignal() {
  if (!sigStatus) return;
  const Conv* c = msgs_conv(sigConv);
  char nm[48];
  emoji_filter(c ? c->name : "", nm, sizeof(nm), false);
  lv_label_set_text(sigTarget, c ? nm : "No one to test with yet");

  const SigResult* r = sig_last();
  bool busy = sig_pending();
  char b[40];
  if (r && r->ok) {
    snprintf(b, sizeof(b), "%.1f s", r->rttMs / 1000.0f);
    lv_label_set_text(sigVal[0], b);
    if (r->hops < 0) snprintf(b, sizeof(b), "flood"); else snprintf(b, sizeof(b), "%d", r->hops);
    lv_label_set_text(sigVal[1], b);
    snprintf(b, sizeof(b), "%d", r->rssi);
    lv_label_set_text(sigVal[2], b);
    snprintf(b, sizeof(b), "%.1f", r->snr4 / 4.0f);
    lv_label_set_text(sigVal[3], b);
  } else {
    for (int i = 0; i < 4; i++) lv_label_set_text(sigVal[i], "-");
  }

  if (busy) {
    snprintf(b, sizeof(b), "Testing... %us", (unsigned)((millis() - sigStartMs) / 1000));
    lv_label_set_text(sigStatus, b);
    lv_obj_set_style_text_color(sigStatus, C(ACCENT), 0);
    lv_label_set_text(sigBtnLbl, "Testing...");
  } else {
    lv_label_set_text(sigBtnLbl, "Send range test");
    if (!r) {
      lv_label_set_text(sigStatus, "Sends a short message and times the reply.");
      lv_obj_set_style_text_color(sigStatus, C(MUTE), 0);
    } else if (r->ok) {
      snprintf(b, sizeof(b), "Delivered. Test %d of this session.", sig_total());
      lv_label_set_text(sigStatus, b);
      lv_obj_set_style_text_color(sigStatus, C(GOOD), 0);
    } else {
      lv_label_set_text(sigStatus, "No reply. Out of range, or no route.");
      lv_obj_set_style_text_color(sigStatus, C(BAD), 0);
    }
  }
}

static void sigSend() {
  if (sig_pending()) return;
  sigPickDefault();
  if (sigConv < 0) { toast("Nobody known yet. Wait for an advert."); return; }
  sigStartMs = millis();
  if (!actions.sendTest || !actions.sendTest(sigConv)) toast("Could not send the test");
  refreshSignal();
}
static void sigSendCb(lv_event_t*) { sigSend(); }

static void pickSig(int i) {
  if (i >= 0 && i < MAX_CONVS) sigConv = sigChoices[i];
  openSignal();
}
static void sigTargetCb(lv_event_t*) {
  int order[MAX_CONVS];
  int n = msgs_sorted(order, MAX_CONVS), m = 0, cur = 0;
  for (int i = 0; i < n && m < 24; i++) {
    const Conv* k = msgs_conv(order[i]);
    if (!k || k->channel) continue;
    if (order[i] == sigConv) cur = m;
    sigChoices[m] = order[i];
    emoji_filter(k->name, pickBuf[m], sizeof(pickBuf[m]), false);
    m++;
  }
  if (m == 0) { toast("Nobody known yet"); return; }
  openPicker("Test with", m, cur, pickSig, M_SIGNAL);
}

static void openSignal() {
  mode = M_SIGNAL;
  returnTo = M_SIGNAL;
  sigPickDefault();
  lv_obj_t* s = newScreen();
  headerFor(s, "Signal");

  lv_obj_t* target = tappable(box(s, 312, 36, RAISE, 12));
  lv_obj_set_pos(target, 4, 60);
  lv_obj_set_style_border_color(target, C(ACCENT), 0);
  lv_obj_set_style_border_width(target, 0, 0);
  lv_obj_t* to = label(target, "To", &font_tdos_12, MUTE);
  lv_obj_align(to, LV_ALIGN_LEFT_MID, 12, 0);
  sigTarget = label(target, "", &font_tdos_14, TEXT);
  lv_obj_align(sigTarget, LV_ALIGN_LEFT_MID, 40, 0);
  lv_obj_set_width(sigTarget, 230);
  lv_label_set_long_mode(sigTarget, LV_LABEL_LONG_CLIP);
  lv_obj_t* chev = label(target, LV_SYMBOL_RIGHT, &font_tdos_14, TEXT);
  lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -10, 0);
  lv_obj_add_event_cb(target, sigTargetCb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* btn = tappable(box(s, 312, 44, ACCENT, 14));
  lv_obj_set_pos(btn, 4, 102);
  sigBtnLbl = label(btn, "Send range test", &font_tdos_16, INK);
  lv_obj_center(sigBtnLbl);
  lv_obj_add_event_cb(btn, sigSendCb, LV_EVENT_CLICKED, nullptr);

  const char* caps[4] = { "ROUND TRIP", "HOPS", "RSSI dBm", "SNR dB" };
  for (int i = 0; i < 4; i++) {
    lv_obj_t* t = box(s, 74, 52, RAISE, 12);
    lv_obj_set_pos(t, 4 + i * 79, 154);
    sigVal[i] = label(t, "-", &font_tdos_16, TEXT);
    lv_obj_align(sigVal[i], LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_t* c = label(t, caps[i], &font_tdos_12, MUTE);
    lv_obj_align(c, LV_ALIGN_BOTTOM_MID, 0, -6);
  }
  sigStatus = label(s, "", &font_tdos_12, MUTE);
  lv_obj_set_pos(sigStatus, 8, 214);
  sigShownVersion = sig_version();
  refreshSignal();
  loadApp(s);
}

// ── building the screens ─────────────────────────────────────────
static void buildStatusBar() {
  bar = box(lv_layer_top(), 320, 22, INK, 0);
  lv_obj_set_pos(bar, 0, 0);
  barTime = label(bar, "--:--", &font_tdos_14, TEXT);
  lv_obj_align(barTime, LV_ALIGN_LEFT_MID, 8, 0);
  barBatt = label(bar, LV_SYMBOL_BATTERY_3 " --%", &font_tdos_12, TEXT);
  lv_obj_align(barBatt, LV_ALIGN_RIGHT_MID, -8, 0);
  barSig = label(bar, LV_SYMBOL_WIFI, &font_tdos_12, MUTE);
  lv_obj_align_to(barSig, barBatt, LV_ALIGN_OUT_LEFT_MID, -10, 0);
}

static void buildLock() {
  scrLock = newScreen();
  lockMesh = label(scrLock, "Starting...", &font_tdos_12, MUTE);
  lv_obj_align(lockMesh, LV_ALIGN_TOP_MID, 0, 8);
  lockTime = label(scrLock, "--:--", &lv_font_montserrat_48, TEXT);
  lv_obj_align(lockTime, LV_ALIGN_CENTER, 0, -18);
  lockDate = label(scrLock, "", &font_tdos_14, MUTE);
  lv_obj_align(lockDate, LV_ALIGN_CENTER, 0, 26);
  lv_obj_t* hint = label(scrLock, "Click, touch or press a key", &font_tdos_12, MUTE);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

static void buildHome() {
  scrHome = newScreen();
  const int colX[3] = { 53, 160, 267 };
  const int rowTop[2] = { 30, 128 };           // tile tops; a label sits just below each tile
  for (int i = 0; i < 6; i++) {
    int cx = colX[i % 3];
    int top = rowTop[i / 3];
    lv_obj_t* t = tappable(box(scrHome, 64, 64, apps[i].col, 18));
    lv_obj_set_pos(t, cx - 32, top);
    lv_obj_set_style_outline_color(t, C(TEXT), 0);
    lv_obj_set_style_outline_pad(t, 2, 0);
    lv_obj_set_style_outline_width(t, 0, 0);
    lv_obj_t* s = label(t, apps[i].sym, &lv_font_montserrat_24, INK);
    lv_obj_center(s);
    lv_obj_add_event_cb(t, tileCb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t* n = label(scrHome, apps[i].name, &font_tdos_14, TEXT);
    lv_obj_update_layout(n);
    lv_obj_set_pos(n, cx - lv_obj_get_width(n) / 2, top + 64 + 6);
    tiles[i] = t;
  }
  // unread badge on the Messages tile
  badge = box(scrHome, 22, 22, BAD, 11);
  lv_obj_set_pos(badge, colX[0] + 32 - 14, rowTop[0] - 8);
  badgeLbl = label(badge, "0", &font_tdos_12, 0xFFFFFF);
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
  emoji_init();                        // inline emoji from the SD card, if the pack is there

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

  lastBatt = m.batteryPct;
  strncpy(lastName, m.nodeName, sizeof(lastName) - 1);
  strncpy(lastStorage, m.storage, sizeof(lastStorage) - 1);

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

  nowUtc = m.clockValid ? m.utc : 0;
  if (mode == M_SIGNAL && sigStatus && (sig_version() != sigShownVersion || sig_pending())) {
    sigShownVersion = sig_version();
    refreshSignal();
  }
  if (mode == M_NODES && nodesBox && (nodes_version() != nodesShownVersion || millis() - nodesBuiltMs > 10000)) {
    nodesShownVersion = nodes_version();
    nodesBuiltMs = millis();
    fillNodes();
  }

  // Redraw Messages screens only when the store actually changed.
  uint32_t v = msgs_version();
  if (v != shownVersion) {
    if (mode == M_CONVS) { fillConvs(); shownVersion = v; }
    else if (mode == M_THREAD) { msgs_mark_read(curConv); buildThread(); shownVersion = msgs_version(); }
  }

  if (mode == M_DBG && dbgLabel) {
    static char flat[720];
    emoji_filter(m.consoleText, flat, sizeof(flat), false);
    if (strcmp(dbgCache, flat) != 0) {
      strncpy(dbgCache, flat, sizeof(dbgCache) - 1);
      lv_label_set_text(dbgLabel, dbgCache);
    }
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
  } else if ((mode == M_SETTINGS || mode == M_ADV || mode == M_PICK) && listN > 0) {
    if (dy) {
      listSel = nextSelectable(listSel, dy > 0 ? 1 : -1);
      listHighlight();
      lv_obj_scroll_to_y(listBox, max(0, listSel * 38 - 76), LV_ANIM_OFF);
    }
  } else if (mode == M_NODES && dy && nodesBox) {
    lv_obj_scroll_by(nodesBox, 0, -dy * 50, LV_ANIM_OFF);
  } else if (mode == M_BRIGHT && dx && brightSlider) {
    int v = constrain(lv_slider_get_value(brightSlider) + dx, 1, 16);
    lv_slider_set_value(brightSlider, v, LV_ANIM_OFF);
    brightApply(v);
  }
}

void ui_click() {
  if (mode == M_LOCK) { ui_activity(); wake(); return; }
  ui_activity();
  if (mode == M_HOME) openApp(sel);
  else if (mode == M_CONVS && convN > 0) openThread(convOrder[convSel]);
  else if (mode == M_THREAD) doSend();
  else if (mode == M_ENTRY) entrySave();
  else if (mode == M_SIGNAL) sigSend();
  else if ((mode == M_SETTINGS || mode == M_ADV || mode == M_PICK) && listN > 0) {
    if (mode == M_SETTINGS) settingsAct(listIds[listSel]);
    else if (mode == M_ADV) advAct(listIds[listSel]);
    else pickChoose(listIds[listSel]);
  }
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
  if (mode == M_ENTRY && taInput) {
    if (c == '\r' || c == '\n') { entrySave(); return; }
    if (c == 8 || c == 127) { if (lv_textarea_get_text(taInput)[0] == 0) goBack(); else lv_textarea_del_char(taInput); return; }
    bool ok = entryNumeric ? ((c >= '0' && c <= '9') || c == '.') : (c >= 32 && c < 127);
    if (ok) lv_textarea_add_char(taInput, (uint32_t)c);
    return;
  }
  if (mode == M_PIN) {
    if (c >= '0' && c <= '9') { ui_pin_key(c == '0' ? 10 : c - '1'); return; }
    if (c == 8 || c == 127) { if (pinLen == 0) goBack(); else ui_pin_key(9); return; }
    return;
  }
  if ((mode == M_APP || mode == M_CONVS || mode == M_DBG || mode == M_SETTINGS || mode == M_ADV || mode == M_PICK || mode == M_BRIGHT || mode == M_SIGNAL || mode == M_NODES) && (c == 8 || c == 127)) { goBack(); return; }
}

bool ui_console_active() { return mode == M_DBG; }
