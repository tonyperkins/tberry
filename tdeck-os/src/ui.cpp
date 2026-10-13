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
#include "mapview.h"
#include "rangetest.h"
#include "fonts/fonts.h"
#include "emoji.h"
#include "audio.h"
#include "reaction.h"
#include "mention.h"
#include "dupcount.h"

extern TFT_eSPI tft;

// How long without a touch, key or trackball movement before the clock screen (and dimming).
static const uint16_t TIMEOUT_SECONDS[] = { 30, 60, 120, 300, 600, 0 };     // 0 = never
static const char* const TIMEOUT_NAMES[] = { "30 seconds", "1 minute", "2 minutes", "5 minutes", "10 minutes", "Never" };
static constexpr int TIMEOUT_N = 6;
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
enum Mode { M_LOCK, M_HOME, M_APP, M_DBG, M_CONVS, M_THREAD, M_SETTINGS, M_ENTRY, M_PIN, M_ADV, M_PICK, M_BRIGHT, M_SIGNAL, M_NODES, M_STATUS, M_MAP, M_DETAIL, M_SETUP };
static Mode mode = M_LOCK;
static uint32_t lastActivityMs = 0;
static bool touchHeld = false;          // a touch that woke the device is ignored until released
static UiSendFn sendFn = nullptr;
enum Gesture { G_NONE, G_BACK, G_HOME, G_QUICK };
static Gesture pendingGesture = G_NONE;     // set by the touch callback, run from ui_tick()

static lv_obj_t *scrLock, *scrHome, *scrApp;        // scrApp: whichever non-home screen is open
static lv_obj_t *bar, *barTime, *barSig, *barBatt;
static lv_obj_t *lockTime, *lockDate, *lockMesh;
static lv_obj_t *tiles[6], *badge, *badgeLbl;
static lv_obj_t *dbgLabel, *convBox, *thrBox, *taInput;
static lv_obj_t *pickerObj, *pickerGrids[4], *pickerTabs[4];   // emoji picker over the thread
static int pickerTab = 0;
static lv_group_t* inputGroup = nullptr;   // gives the message box the focus state that shows its cursor
static lv_obj_t* listBox;                // scrolling list used by Settings and Advanced
static int listN = 0, listSel = 0;
static lv_obj_t *pinMsg, *pinDots[4];
static lv_obj_t *brightSlider, *brightLbl;
static int levelKind = 0;                // slider screen: 0 brightness, 1 volume
static lv_obj_t *barGps, *barGpsX;
static lv_obj_t *nodesBox;
static lv_obj_t *stVal[12], *stSub[12], *stPill, *stPillLbl, *stNote, *stBox;
static UiModel stLast;                   // the latest model, for screens built after it arrived
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
static constexpr int THREAD_WINDOW = 36;       // newest messages drawn; older ones stay in the store
static lv_obj_t* threadRows[MSGS_PER_CONV];
static int mapReturnConv = -1;
static int detailConv = -1, detailIdx = -1;     // message shown on the details sheet           // conversation to go back to when the map was opened from a message
static uint32_t shownVersion = 0;
static char dbgCache[720];
static int unreadShown = -1;

struct App { const char* name; const char* sym; uint32_t col; const char* blurb; };
static const App apps[6] = {
  { "Messages", LV_SYMBOL_ENVELOPE, 0x4C8DFF, "" },
  { "Signal", LV_SYMBOL_WIFI, 0xFFB547, "" },
  { "Nodes", LV_SYMBOL_LIST, 0x2FC4B2, "" },
  { "Map", LV_SYMBOL_GPS, 0x8B7CFF, "" },
  { "Status", LV_SYMBOL_BATTERY_3, 0x43C77B, "" },
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
static void toast(const char* text);
static void showHome();
static void openApp(int i);
static void openSetup(int step);
static void openMap();
static void openMapFocus(double lat, double lon, const char* name);
static void openMessageDetails(int conv, int idx);
static void openNodes();
static void openStatus();
static void openConvs();
static void openThread(int conv);
static void goBack();

static void setSel(int i) {
  sel = constrain(i, 0, 5);
  for (int k = 0; k < 6; k++) lv_obj_set_style_outline_width(tiles[k], k == sel ? 3 : 0, 0);
}

static void releaseApp() {            // forget pointers into the app screen before it goes away
  dbgLabel = nullptr; convBox = nullptr; thrBox = nullptr; taInput = nullptr; pickerObj = nullptr; for (auto& g : pickerGrids) g = nullptr;
  if (mapview_active()) mapview_destroy();
  listBox = nullptr; listN = 0; pinMsg = nullptr; resetArmed = false;
  brightSlider = nullptr; brightLbl = nullptr; nodesBox = nullptr; stBox = nullptr; stPill = nullptr; stNote = nullptr; sigTarget = nullptr; sigBtnLbl = nullptr; sigStatus = nullptr;
  curConv = -1;
  msgs_set_open(-1);
}

// Where the user was when the screen locked, so waking returns there instead of to Home.
// Advanced settings (PIN) and edit screens are not resumed: they come back as Settings.
static int setupStep = 0;
static Mode resumeMode = M_HOME;
static int resumeConv = -1;
static char resumeDraft[160];
static void openSettings();
static void openSignal();

static void rememberPlace() {
  resumeMode = M_HOME;
  resumeConv = -1;
  resumeDraft[0] = 0;
  switch (mode) {
    case M_THREAD:
      resumeMode = M_THREAD;
      resumeConv = curConv;
      if (taInput) snprintf(resumeDraft, sizeof(resumeDraft), "%s", lv_textarea_get_text(taInput));
      break;
    case M_DETAIL: resumeMode = M_THREAD; resumeConv = detailConv; break;
    case M_CONVS: case M_SIGNAL: case M_NODES: case M_STATUS: case M_SETTINGS: resumeMode = mode; break;
    case M_MAP: if (mapReturnConv < 0) resumeMode = M_MAP; break;
    case M_ENTRY: case M_PICK: case M_BRIGHT: case M_ADV: case M_PIN: resumeMode = M_SETTINGS; break;
    default: break;                                    // Home, console, placeholders
  }
}

static void wake() {
  setBacklight(cfg().brightness);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
  if (!cfg().setupDone) { openSetup(setupStep); return; }   // first boot, or locked part-way
  Mode m = resumeMode;
  resumeMode = M_HOME;
  switch (m) {
    case M_THREAD:
      if (resumeConv >= 0 && msgs_conv(resumeConv)) {
        openThread(resumeConv);
        if (resumeDraft[0] && taInput) lv_textarea_set_text(taInput, resumeDraft);
        return;
      }
      openConvs();
      return;
    case M_CONVS: openConvs(); return;
    case M_SIGNAL: openSignal(); return;
    case M_NODES: openNodes(); return;
    case M_STATUS: openStatus(); return;
    case M_SETTINGS: openSettings(); return;
    case M_MAP: openMap(); return;
    default: showHome(); return;
  }
}

static void closeQuick(bool immediate);
static void goLock() {
  closeQuick(true);
  rememberPlace();
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
static bool closePicker() {
  if (!pickerObj) return false;
  lv_obj_del(pickerObj);
  pickerObj = nullptr; for (auto& g : pickerGrids) g = nullptr;
  return true;
}
static void goBack() {
  if (closePicker()) return;                          // back closes the emoji picker first
  if (mode == M_MAP && mapReturnConv >= 0) {          // from a message: back to that conversation
    int c = mapReturnConv;
    mapReturnConv = -1;
    openThread(c);
    return;
  }
  if (mode == M_THREAD) { openConvs(); return; }
  if (mode == M_DETAIL) { int c = detailConv; detailConv = -1; openThread(c); return; }
  if (mode == M_PICK && returnTo == M_SETUP) { openSetup(2); return; }
  if (mode == M_SETUP) return;                         // the setup has its own Back button
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
  if (i == 4) { openStatus(); return; }
  if (i == 3) { openMap(); return; }
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
    if (last) {
      char tmp[230];
      char ra[40], re[24], rc[12];
      if (c->channel && !last->out && parseReaction(last->text, ra, sizeof(ra), re, sizeof(re), rc, sizeof(rc)))
        snprintf(tmp, sizeof(tmp), "%s reacted %s", last->sender[0] ? last->sender : "Someone", re);
      else if (c->channel && last->sender[0] && !last->out) snprintf(tmp, sizeof(tmp), "%s: %s", last->sender, last->text);
      else snprintf(tmp, sizeof(tmp), "%s", last->text);
      emoji_filter(tmp, pv, sizeof(pv), true);
    }
    lv_obj_t* p = label(row, pv, emoji_font(&font_tdos_12), MUTE);
    lv_obj_set_pos(p, 46, 25);
    lv_obj_set_width(p, 200);
    lv_label_set_long_mode(p, LV_LABEL_LONG_CLIP);
    if (c->unread) {
      lv_obj_t* d = box(row, 22, 22, ACCENT, 11);
      lv_obj_set_pos(d, 282, 12);
      char u[8];
      if (c->mentionUnread) snprintf(u, sizeof(u), "@");        // someone is talking to you
      else snprintf(u, sizeof(u), "%d", c->unread > 99 ? 99 : c->unread);
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

// "3 hops", "1 hop", "direct" (heard straight from the sender) or "routed" (a direct-routed packet,
// whose hop list was consumed on the way, so the count is unknown).
static void formatHops(const Msg* m, char* out, size_t n) {
  out[0] = 0;
  if (!(m->flags & MF_RADIO)) return;
  if (!(m->flags & MF_FLOOD)) { snprintf(out, n, "routed"); return; }
  if (m->hops == 0) snprintf(out, n, "direct");
  else if (m->hops == 1) snprintf(out, n, "1 hop");
  else snprintf(out, n, "%d hops", m->hops);
}
// SNR in quarter-dB steps, printed without trailing zeros: 7.25, 6.5, -0.75, 11.
static void formatSnr(int8_t snr4, char* out, size_t n) {
  float v = snr4 / 4.0f;
  snprintf(out, n, "%.2f", v);
  size_t l = strlen(out);
  while (l > 1 && out[l - 1] == '0') out[--l] = 0;
  if (l > 1 && out[l - 1] == '.') out[--l] = 0;
}

// Reply: put @[Sender] at the start of the message box, replacing a mention already there so one
// tap per person switches who the reply is for. The name goes in exactly as it was transmitted.
static void insertMention(const char* sender) {
  if (!taInput || !sender || !sender[0]) return;
  const char* cur = lv_textarea_get_text(taInput);
  const char* rest = cur;
  if (strncmp(cur, "@[", 2) == 0) {
    const char* close = strchr(cur, ']');
    if (close) { rest = close + 1; if (*rest == ' ') rest++; }
  }
  char buf[220];
  snprintf(buf, sizeof(buf), "@[%s] %s", sender, rest);
  if (strlen(buf) > 140) { toast("Too long to add a mention"); return; }
  lv_textarea_set_text(taInput, buf);
  lv_textarea_set_cursor_pos(taInput, LV_TEXTAREA_CURSOR_LAST);
}
static void bubbleHoldCb(lv_event_t* e) {
  openMessageDetails(curConv, (int)(intptr_t)lv_event_get_user_data(e));
}
static void bubbleTapCb(lv_event_t* e) {
  const Conv* c = msgs_conv(curConv);
  const Msg* m = msgs_at(curConv, (int)(intptr_t)lv_event_get_user_data(e));
  if (!c || !c->channel || !m || m->out || !m->sender[0]) return;   // only replies to others, in a channel
  insertMention(m->sender);
}

// Do a mention's name and a message's raw sender name refer to the same person? The stored text has
// been through the sanitiser (joiners removed), so compare against the sanitised sender as well.
static bool sameName(const char* mention, const char* rawSender) {
  if (!rawSender[0]) return false;
  if (strcmp(mention, rawSender) == 0) return true;
  char cleaned[40];
  msgs_clean(cleaned, sizeof(cleaned), rawSender);
  return strcmp(mention, cleaned) == 0;
}

// The message a reply at index `i` is answering: the latest earlier message from the person it
// mentions (or from us, when someone has mentioned this node), skipping reactions.
static int findParent(int conv, int i, const char* mentioned) {
  bool aboutUs = lastName[0] && sameName(mentioned, lastName);
  for (int j = i - 1; j >= 0 && j >= i - 80; j--) {
    const Msg* p = msgs_at(conv, j);
    if (!p) break;
    char a[40], e[24], cd[12];
    if (parseReaction(p->text, a, sizeof(a), e, sizeof(e), cd, sizeof(cd))) continue;
    if (aboutUs ? p->out : (!p->out && sameName(mentioned, p->sender))) return j;
  }
  return -1;
}

static void quoteTapCb(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx >= 0 && idx < MSGS_PER_CONV && threadRows[idx]) lv_obj_scroll_to_view(threadRows[idx], LV_ANIM_ON);
}

// The position a message's sender advertised, if we know one. Channel messages carry only the
// sender's name, so those match by name; direct messages are exact, by key.
static bool messagePosition(const Conv* c, const Msg* m, int32_t* lat, int32_t* lon, char* name, size_t n) {
  if (m->out) return false;
  if (c->channel) {
    if (!m->sender[0] || !nodes_position_for_name(m->sender, lat, lon)) return false;
    snprintf(name, n, "%s", m->sender);
    return true;
  }
  if (!nodes_position_for_key(c->key, lat, lon)) return false;
  snprintf(name, n, "%s", c->name);
  return true;
}
static void showOnMapCb(lv_event_t* e) {
  const Conv* c = msgs_conv(curConv);
  const Msg* m = msgs_at(curConv, (int)(intptr_t)lv_event_get_user_data(e));
  int32_t la, lo;
  char who[40];
  if (!c || !m || !messagePosition(c, m, &la, &lo, who, sizeof(who))) return;
  mapReturnConv = curConv;
  char shown[48];
  emoji_filter(who, shown, sizeof(shown), false);   // the map's label font has no emoji
  openMapFocus(la / 1e6, lo / 1e6, shown);
}

static void buildThread() {
  if (!thrBox) return;
  lv_obj_clean(thrBox);
  memset(threadRows, 0, sizeof(threadRows));
  const Conv* c = msgs_conv(curConv);
  if (!c) return;
  int first = max(0, c->count - THREAD_WINDOW);

  // Reactions first: each one is attached to the latest earlier message from the author it names,
  // which is how other apps resolve them (the 8 character code is not yet understood, TDOS-008).
  static char badges[THREAD_WINDOW][112];
  static bool isBadgedReaction[THREAD_WINDOW];
  memset(badges, 0, sizeof(badges));
  memset(isBadgedReaction, 0, sizeof(isBadgedReaction));
  if (c->channel) {
    for (int r = first; r < c->count; r++) {
      const Msg* rm = msgs_at(curConv, r);
      char ra[40], re[24], rc[12];
      if (!rm || rm->out || !parseReaction(rm->text, ra, sizeof(ra), re, sizeof(re), rc, sizeof(rc))) continue;
      int target = findParent(curConv, r, ra);
      if (target < first || target >= c->count) continue;       // not on screen: keep the plain line
      isBadgedReaction[r - first] = true;
      char who[40];
      emoji_filter(rm->sender[0] ? rm->sender : "?", who, sizeof(who), true);
      char one[80];
      snprintf(one, sizeof(one), "%s %s", re, who);
      char* b = badges[target - first];
      if (strlen(b) + strlen(one) + 4 < sizeof(badges[0])) {
        if (b[0]) strcat(b, "   ");
        strcat(b, one);
      }
    }
  }

  for (int i = first; i < c->count; i++) {
    const Msg* m = msgs_at(curConv, i);

    // A reaction (@[Author]<emoji> <code>) is not a bubble: one quiet line, without the code.
    char rAuthor[40], rEmoji[24], rCode[12];
    if (c->channel && !m->out && isBadgedReaction[i - first]) continue;   // shown as a badge on its target
    if (c->channel && !m->out && parseReaction(m->text, rAuthor, sizeof(rAuthor), rEmoji, sizeof(rEmoji), rCode, sizeof(rCode))) {
      char raw[160], shown[160];
      const char* who = m->sender[0] ? m->sender : "Someone";
      bool toMe = lastName[0] && sameName(rAuthor, lastName);
      snprintf(raw, sizeof(raw), "%s reacted %s to %s", who, rEmoji, toMe ? "you" : rAuthor);
      emoji_filter(raw, shown, sizeof(shown), true);   // the emoji if the pack has it, else just the words
      lv_obj_t* rl = label(thrBox, shown, emoji_font(&font_tdos_12), toMe ? ACCENT : MUTE);
      lv_obj_set_width(rl, lv_pct(100));
      lv_label_set_long_mode(rl, LV_LABEL_LONG_WRAP);
      continue;
    }

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
    threadRows[i] = row;

    bool channelIn = !m->out && c->channel && m->sender[0];
    if (channelIn) {
      char who[48];
      emoji_filter(m->sender, who, sizeof(who), true);
      lv_obj_t* cap = label(row, who, emoji_font(&font_tdos_12), avatarColor(m->sender));
      lv_obj_set_style_max_width(cap, 236, 0);
      lv_label_set_long_mode(cap, LV_LABEL_LONG_CLIP);
    }

    // A reply in a channel starts "@[Name]": show what it answers, as other apps do (TDOS-007).
    char qName[40];
    if (c->channel && leadingMention(m->text, qName, sizeof(qName))) {
      int pj = findParent(curConv, i, qName);
      char qShown[160], who[48];
      emoji_filter(qName, who, sizeof(who), true);
      if (pj >= 0) snprintf(qShown, sizeof(qShown), "%s: %s", who, msgs_at(curConv, pj)->text);
      else snprintf(qShown, sizeof(qShown), "reply to %s", who);
      char qFiltered[170];
      emoji_filter(qShown, qFiltered, sizeof(qFiltered), true);
      lv_obj_t* q = lv_label_create(row);
      lv_label_set_text(q, qFiltered);
      lv_obj_set_style_text_font(q, emoji_font(&font_tdos_12), 0);
      lv_obj_set_style_text_color(q, C(MUTE), 0);
      lv_label_set_long_mode(q, LV_LABEL_LONG_DOT);        // ends in "..." instead of running on
      lv_obj_set_width(q, 224);
      lv_obj_set_style_border_side(q, LV_BORDER_SIDE_LEFT, 0);
      lv_obj_set_style_border_width(q, 2, 0);
      lv_obj_set_style_border_color(q, C(pj >= 0 ? ACCENT : LINE), 0);
      lv_obj_set_style_pad_left(q, 6, 0);
      if (pj >= first && pj >= 0) {                        // the parent is on screen: tap to jump to it
        lv_obj_add_flag(q, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(q, quoteTapCb, LV_EVENT_CLICKED, (void*)(intptr_t)pj);
      }
    }

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
    if (m->flags & MF_MENTION) {                       // someone addressed this node by name
      lv_obj_set_style_border_width(b, 2, 0);
      lv_obj_set_style_border_color(b, C(ACCENT), 0);
    }
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (channelIn)                                     // short tap = reply to this person
      lv_obj_add_event_cb(b, bubbleTapCb, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)i);
    lv_obj_add_event_cb(b, bubbleHoldCb, LV_EVENT_LONG_PRESSED, (void*)(intptr_t)i);   // hold = details

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
      char hp[16], sn[16];
      formatHops(m, hp, sizeof(hp));
      if (hp[0]) {
        formatSnr(m->snr4, sn, sizeof(sn));
        snprintf(meta, sizeof(meta), "%s  %s  SNR %s", when, hp, sn);
      } else {
        snprintf(meta, sizeof(meta), "%s", when);
      }
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
    if (badges[i - first][0]) {
      char shownBadges[120];
      emoji_filter(badges[i - first], shownBadges, sizeof(shownBadges), true);
      lv_obj_t* bl = label(row, shownBadges, emoji_font(&font_tdos_12), ACCENT);
      lv_obj_set_style_max_width(bl, 236, 0);
    }
    if (meta[0]) {
      // A crosshair after the line means the sender's position is known: tap it to see where.
      int32_t la, lo;
      char who[40];
      if (!m->out && messagePosition(c, m, &la, &lo, who, sizeof(who))) {
        strncat(meta, "  " LV_SYMBOL_GPS, sizeof(meta) - strlen(meta) - 1);
        lv_obj_t* ml = label(row, meta, &font_tdos_12, ACCENT);
        lv_obj_set_style_pad_all(ml, 5, 0);            // a finger-sized target
        lv_obj_add_flag(ml, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(ml, showOnMapCb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
      } else {
        label(row, meta, &font_tdos_12, metaCol);
      }
    }
  }
  lv_obj_update_layout(thrBox);
  lv_obj_scroll_to_y(thrBox, LV_COORD_MAX, LV_ANIM_OFF);
}


// ── Emoji picker: a panel over the thread with a few pages of common emoji ──
// Only emoji that have an image on the card are listed. Symbols in the old BMP range get a
// variation selector appended so other clients show them as emoji, not as text glyphs.
static const uint32_t PICK_HANDS[] = { 0x1F44D, 0x1F44E, 0x1F44F, 0x1F64C, 0x1F64F, 0x1F44B, 0x1F91D, 0x270C, 0x1F44C, 0x1F4AA,
  0x1F44A, 0x270A, 0x270B, 0x1F918, 0x1F91E, 0x1F449, 0x1F448, 0x1F446, 0x1F447, 0x261D, 0x1F590, 0x1F91A, 0x1F485, 0x1F440,
  0x1F9E0, 0x1F64B, 0x1F926, 0x1F937, 0x1F645, 0x1F646, 0x1F481, 0x1F6B6, 0x1F3C3, 0x1F483, 0x1F57A };
static const uint32_t PICK_SYMBOLS[] = { 0x2764, 0x1F9E1, 0x1F49B, 0x1F49A, 0x1F499, 0x1F49C, 0x1F5A4, 0x1F90D, 0x1F494, 0x1F495,
  0x1F496, 0x1F4AF, 0x2728, 0x1F525, 0x2B50, 0x1F31F, 0x26A1, 0x2705, 0x274C, 0x2757, 0x2753, 0x26A0, 0x1F389, 0x1F38A,
  0x1F4A5, 0x1F4A4, 0x1F4A6, 0x1F6D1, 0x2139, 0x1F51D, 0x1F198, 0x1F197, 0x1F192, 0x27A1, 0x2B05, 0x2B06, 0x2B07 };
static const uint32_t PICK_THINGS[] = { 0x1F4E1, 0x1F4FB, 0x1F50B, 0x1F4CD, 0x1F9ED, 0x1F5FA, 0x1F3D4, 0x1F332, 0x1F333, 0x2600,
  0x1F327, 0x26C8, 0x1F308, 0x2744, 0x1F30A, 0x1F697, 0x1F699, 0x1F69B, 0x1F3D5, 0x1F3E0, 0x1F3E2, 0x2615, 0x1F37A, 0x1F355,
  0x1F354, 0x1F32E, 0x1F36A, 0x1F3B5, 0x1F3AE, 0x1F4F7, 0x1F4F1, 0x1F4BB, 0x1F527, 0x1F511, 0x1F4A1, 0x23F0, 0x1F6A8 };

static int pickerList(int tab, uint32_t* out, int cap) {
  int n = 0;
  auto add = [&](uint32_t cp) { if (n < cap && emoji_available(cp)) out[n++] = cp; };
  if (tab == 0) {
    for (uint32_t c = 0x1F600; c <= 0x1F64F; c++) add(c);
    for (uint32_t c = 0x1F910; c <= 0x1F92F; c++) add(c);
    for (uint32_t c = 0x1F970; c <= 0x1F97A; c++) add(c);
  } else {
    const uint32_t* l = tab == 1 ? PICK_HANDS : tab == 2 ? PICK_SYMBOLS : PICK_THINGS;
    int len = tab == 1 ? (int)(sizeof(PICK_HANDS) / 4) : tab == 2 ? (int)(sizeof(PICK_SYMBOLS) / 4) : (int)(sizeof(PICK_THINGS) / 4);
    for (int i = 0; i < len; i++) add(l[i]);
  }
  return n;
}

static void pickEmojiCb(lv_event_t* e) {
  if (!taInput) return;
  uint32_t cp = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  char u[8];
  int n = 0;
  if (cp >= 0x10000) {
    u[n++] = (char)(0xF0 | (cp >> 18)); u[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
    u[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[n++] = (char)(0x80 | (cp & 0x3F));
  } else {
    u[n++] = (char)(0xE0 | (cp >> 12)); u[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[n++] = (char)(0x80 | (cp & 0x3F));
    u[n++] = (char)0xEF; u[n++] = (char)0xB8; u[n++] = (char)0x8F;       // U+FE0F: show as emoji
  }
  u[n] = 0;
  lv_textarea_add_text(taInput, u);
}

// Only the page on show exists. LVGL's memory pool is small (about 70 KB free), so the cells share
// two styles instead of each carrying its own, and the other pages are freed on a tab switch.
static void pickerFill() {
  if (!pickerObj) return;
  for (int t = 0; t < 4; t++) {
    if (pickerGrids[t] && t != pickerTab) { lv_obj_del(pickerGrids[t]); pickerGrids[t] = nullptr; }
    if (pickerTabs[t]) {
      lv_obj_set_style_bg_color(pickerTabs[t], C(t == pickerTab ? RAISE2 : RAISE), 0);
      lv_obj_set_style_border_width(pickerTabs[t], t == pickerTab ? 2 : 0, 0);
    }
  }
  if (pickerGrids[pickerTab]) return;

  static lv_style_t gridSt, cellSt, cellPressSt, textSt;
  static bool stylesReady = false;
  const lv_font_t* f = emoji_font(&font_tdos_16);
  if (!stylesReady) {
    stylesReady = true;
    lv_style_init(&gridSt);
    lv_style_set_width(&gridSt, 320);
    lv_style_set_height(&gridSt, 122 - 34);
    lv_style_set_bg_opa(&gridSt, LV_OPA_TRANSP);
    lv_style_set_border_width(&gridSt, 0);
    lv_style_set_pad_all(&gridSt, 4);
    lv_style_set_pad_row(&gridSt, 2);
    lv_style_set_pad_column(&gridSt, 2);
    lv_style_init(&cellSt);
    lv_style_set_width(&cellSt, 30);
    lv_style_set_height(&cellSt, 30);
    lv_style_set_bg_opa(&cellSt, LV_OPA_TRANSP);
    lv_style_set_border_width(&cellSt, 0);
    lv_style_set_pad_all(&cellSt, 0);
    lv_style_set_radius(&cellSt, 8);
    lv_style_init(&cellPressSt);
    lv_style_set_bg_color(&cellPressSt, C(RAISE2));
    lv_style_set_bg_opa(&cellPressSt, LV_OPA_COVER);
    lv_style_init(&textSt);
    lv_style_set_text_color(&textSt, C(TEXT));
  }
  lv_style_set_text_font(&textSt, f);

  lv_obj_t* grid = lv_obj_create(pickerObj);
  pickerGrids[pickerTab] = grid;
  lv_obj_remove_style_all(grid);
  lv_obj_add_style(grid, &gridSt, 0);
  lv_obj_set_pos(grid, 0, 32);
  lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  uint32_t list[80];
  int n = pickerList(pickerTab, list, 80);
  char one[8];
  for (int i = 0; i < n; i++) {
    uint32_t cp = list[i];
    lv_obj_t* cell = lv_obj_create(grid);
    lv_obj_remove_style_all(cell);
    lv_obj_add_style(cell, &cellSt, 0);
    lv_obj_add_style(cell, &cellPressSt, LV_STATE_PRESSED);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    int k = 0;
    if (cp >= 0x10000) {
      one[k++] = (char)(0xF0 | (cp >> 18)); one[k++] = (char)(0x80 | ((cp >> 12) & 0x3F));
      one[k++] = (char)(0x80 | ((cp >> 6) & 0x3F)); one[k++] = (char)(0x80 | (cp & 0x3F));
    } else {
      one[k++] = (char)(0xE0 | (cp >> 12)); one[k++] = (char)(0x80 | ((cp >> 6) & 0x3F)); one[k++] = (char)(0x80 | (cp & 0x3F));
    }
    one[k] = 0;
    lv_obj_t* l = lv_label_create(cell);
    lv_obj_add_style(l, &textSt, 0);
    lv_label_set_text(l, one);
    lv_obj_center(l);
    lv_obj_add_event_cb(cell, pickEmojiCb, LV_EVENT_CLICKED, (void*)(uintptr_t)cp);
  }
}

static void pickerTabCb(lv_event_t* e) {
  pickerTab = (int)(intptr_t)lv_event_get_user_data(e);
  pickerFill();
}

static void togglePicker() {
  if (closePicker()) return;
  lv_obj_t* s = lv_scr_act();
  pickerObj = box(s, 320, 122, RAISE, 0);
  lv_obj_set_pos(pickerObj, 0, 58);
  lv_obj_set_style_border_width(pickerObj, 1, 0);
  lv_obj_set_style_border_color(pickerObj, C(LINE), 0);
  lv_obj_add_flag(pickerObj, LV_OBJ_FLAG_CLICKABLE);       // taps on the panel never reach the thread
  static const uint32_t tabCp[4] = { 0x1F600, 0x1F44D, 0x2764, 0x1F4E1 };
  const lv_font_t* f = emoji_font(&font_tdos_16);
  for (int t = 0; t < 4; t++) {
    lv_obj_t* b = tappable(box(pickerObj, 78, 28, RAISE, 8));
    lv_obj_set_pos(b, 4 + t * 79, 2);
    lv_obj_set_style_border_color(b, C(ACCENT), 0);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
    char u[8];
    int k = 0;
    uint32_t cp = tabCp[t];
    if (cp >= 0x10000) {
      u[k++] = (char)(0xF0 | (cp >> 18)); u[k++] = (char)(0x80 | ((cp >> 12) & 0x3F));
      u[k++] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[k++] = (char)(0x80 | (cp & 0x3F));
    } else {
      u[k++] = (char)(0xE0 | (cp >> 12)); u[k++] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[k++] = (char)(0x80 | (cp & 0x3F));
    }
    u[k] = 0;
    lv_obj_center(label(b, u, f, TEXT));
    lv_obj_add_event_cb(b, pickerTabCb, LV_EVENT_CLICKED, (void*)(intptr_t)t);
    pickerTabs[t] = b;
  }
  pickerFill();
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
  const int taW = emoji_pack_present() ? 212 : 250;   // room for the emoji button
  lv_obj_set_size(taInput, taW, 52);                  // two lines of 16 pt text
  lv_obj_set_pos(taInput, 6, 240 - 56);
  lv_textarea_set_one_line(taInput, false);
  lv_textarea_set_max_length(taInput, 140);
  lv_textarea_set_placeholder_text(taInput, "Message");
  lv_obj_set_style_bg_color(taInput, C(RAISE), 0);
  lv_obj_set_style_bg_opa(taInput, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(taInput, C(TEXT), 0);
  lv_obj_set_style_text_font(taInput, emoji_font(&font_tdos_16), 0);
  lv_obj_set_style_pad_left(taInput, 10, 0);
  lv_obj_set_style_pad_right(taInput, 30, 0);   // room for the clear button
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

  if (emoji_pack_present()) {
    pickerTab = 0;
    lv_obj_t* eb = tappable(box(s, 36, 52, RAISE, 14));
    lv_obj_set_pos(eb, 6 + taW + 4, 240 - 56);
    lv_obj_set_style_border_width(eb, 1, 0);
    lv_obj_set_style_border_color(eb, C(LINE), 0);
    lv_obj_center(label(eb, "\xF0\x9F\x98\x80", emoji_font(&font_tdos_16), TEXT));
    lv_obj_add_event_cb(eb, [](lv_event_t*) { togglePicker(); }, LV_EVENT_CLICKED, nullptr);
  }

  // Clear button inside the right edge of the box, shown only when there is text.
  lv_obj_t* clr = tappable(box(s, 28, 28, RAISE2, 14));
  lv_obj_set_pos(clr, 6 + taW - 32, 240 - 56 + 12);
  lv_obj_center(label(clr, LV_SYMBOL_CLOSE, &font_tdos_14, MUTE));
  lv_obj_add_flag(clr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(clr, [](lv_event_t* e) {
    if (taInput) lv_textarea_set_text(taInput, "");
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(taInput, [](lv_event_t* e) {
    lv_obj_t* b = (lv_obj_t*)lv_event_get_user_data(e);
    const char* t = lv_textarea_get_text(lv_event_get_target(e));
    if (t && *t) lv_obj_clear_flag(b, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
  }, LV_EVENT_VALUE_CHANGED, clr);

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

enum { ROW_NAME, ROW_TZ, ROW_DST, ROW_GPS, ROW_BRIGHT, ROW_VOLUME, ROW_CHSOUND, ROW_TIMEOUT, ROW_RADIO, ROW_BATT, ROW_ABOUT, ROW_ADV };
enum { AD_PRESET, AD_FREQ, AD_BW, AD_SF, AD_CR, AD_PWR, AD_ADVERT, AD_PIN, AD_SETUP, AD_RESET, AD_INFO };

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
static void detailAct(int id);

static void listRowCb(lv_event_t* e) {
  int id = (int)(intptr_t)lv_event_get_user_data(e);
  for (int i = 0; i < listN; i++) if (listIds[i] == id) listSel = i;
  if (mode == M_SETTINGS) settingsAct(id);
  else if (mode == M_ADV) advAct(id);
  else if (mode == M_PICK) pickChoose(id);
  else if (mode == M_DETAIL) detailAct(id);
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
static void brightApply(int v) {
  char b[16];
  if (levelKind == 0) {
    int level = constrain(v, 1, 16);
    cfg().brightness = (uint8_t)level;
    setBacklight(level);
    snprintf(b, sizeof(b), "%d%%", level * 100 / 16);
  } else {
    int step = constrain(v, 0, 10);                       // eleven steps: muted, 10%, ... 100%
    cfg().volume = (uint8_t)(step * 10);
    audio_set_volume(cfg().volume);
    if (step == 0) snprintf(b, sizeof(b), "Muted");
    else snprintf(b, sizeof(b), "%d%%", step * 10);
  }
  settings_changed();
  if (brightLbl) lv_label_set_text(brightLbl, b);
}
static void brightCb(lv_event_t* e) {
  brightApply(lv_slider_get_value(lv_event_get_target(e)));
}
static void brightReleaseCb(lv_event_t*) {
  if (levelKind == 1) audio_play(SND_BLIP);                // let the user hear the level they chose
}

static void openLevel(int kind) {
  mode = M_BRIGHT;
  returnTo = M_SETTINGS;
  levelKind = kind;
  lv_obj_t* s = newScreen();
  headerFor(s, kind == 0 ? "Brightness" : "Volume");
  char b[16];
  int lo = kind == 0 ? 1 : 0, hi = kind == 0 ? 16 : 10;
  int cur = kind == 0 ? cfg().brightness : cfg().volume / 10;
  if (kind == 0) snprintf(b, sizeof(b), "%d%%", cur * 100 / 16);
  else if (cur == 0) snprintf(b, sizeof(b), "Muted");
  else snprintf(b, sizeof(b), "%d%%", cur * 10);
  brightLbl = label(s, b, &lv_font_montserrat_48, TEXT);
  lv_obj_align(brightLbl, LV_ALIGN_CENTER, 0, -20);
  brightSlider = lv_slider_create(s);
  lv_obj_set_size(brightSlider, 270, 18);
  lv_obj_align(brightSlider, LV_ALIGN_CENTER, 0, 44);
  lv_slider_set_range(brightSlider, lo, hi);
  lv_slider_set_value(brightSlider, cur, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(brightSlider, C(RAISE2), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(brightSlider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(brightSlider, C(ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(brightSlider, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(brightSlider, C(TEXT), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(brightSlider, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_pad_all(brightSlider, 6, LV_PART_KNOB);
  lv_obj_add_event_cb(brightSlider, brightCb, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(brightSlider, brightReleaseCb, LV_EVENT_RELEASED, nullptr);
  lv_obj_t* hint = label(s, "Drag the bar, or roll the trackball sideways", &font_tdos_12, MUTE);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);
  loadApp(s);
}

// ── Pickers' value callbacks ─────────────────────────────────────
static void pickChannelSound(int i) {
  cfg().channelSound = (uint8_t)constrain(i, 0, 2);
  settings_changed();
  openSettings();
}
static void pickTimeout(int i) {
  cfg().screenTimeout = (uint8_t)constrain(i, 0, TIMEOUT_N - 1);
  settings_changed();
  openSettings();
}

// ── First-boot setup: welcome, name, time zone, a few tips ───────
static void setupNext();
static void setupBackCb(lv_event_t*) { if (setupStep > 0) openSetup(setupStep - 1); }
static void setupNextCb(lv_event_t*) { setupNext(); }

static void pickSetupZone(int i) {
  cfg().tzMinutes = zones[constrain(i, 0, ZONE_N - 1)].minutes;
  settings_changed();
  openSetup(2);
}
static void setupZoneCb(lv_event_t*) {
  char off[16];
  for (int i = 0; i < ZONE_N; i++) {
    fmtOffset(zones[i].minutes, off, sizeof(off));
    snprintf(pickBuf[i], sizeof(pickBuf[i]), "%s  %s", zones[i].name, off);
  }
  openPicker("Time zone", ZONE_N, zoneIndex(), pickSetupZone, M_SETUP);
}
static void setupDstCb(lv_event_t*) {
  cfg().dst = cfg().dst ? 0 : 1;
  settings_changed();
  openSetup(2);
}

static void setupNext() {
  if (setupStep == 1) {
    if (!taInput) return;
    char clean[32];
    strncpy(clean, lv_textarea_get_text(taInput), sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = 0;
    int n = strlen(clean);
    while (n > 0 && clean[n - 1] == ' ') clean[--n] = 0;
    if (n == 0) { toast("Please enter a name"); return; }
    strncpy(cfg().name, clean, sizeof(cfg().name) - 1);
    settings_changed();
    if (actions.setName) actions.setName(clean);
    strncpy(lastName, clean, sizeof(lastName) - 1);
    openSetup(2);
  } else if (setupStep >= 3) {
    cfg().setupDone = 1;
    settings_changed();
    showHome();
  } else {
    openSetup(setupStep + 1);
  }
}

static void openSetup(int step) {
  mode = M_SETUP;
  setupStep = constrain(step, 0, 3);
  taInput = nullptr;
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t* s = newScreen();
  static const char* const titles[4] = { "Welcome", "Your name", "Time zone", "All set" };
  lv_obj_t* t = label(s, titles[setupStep], &font_tdos_20, TEXT);
  lv_obj_set_pos(t, 16, 40);
  char pg[12];
  snprintf(pg, sizeof(pg), "%d of 4", setupStep + 1);
  lv_obj_t* pl = label(s, pg, &font_tdos_12, MUTE);
  lv_obj_align(pl, LV_ALIGN_TOP_RIGHT, -16, 46);

  auto button = [&](const char* text, int x, int w, uint32_t col, uint32_t txt, lv_event_cb_t cb) {
    lv_obj_t* b = tappable(box(s, w, 40, col, 14));
    lv_obj_set_pos(b, x, 194);
    lv_obj_center(label(b, text, &font_tdos_16, txt));
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  };

  if (setupStep == 0) {
    lv_obj_t* l = label(s, "A mesh messenger that works on its own, no phone needed.\n\nA few quick questions first.", &font_tdos_14, TEXT);
    lv_obj_set_width(l, 288);
    lv_obj_set_pos(l, 16, 80);
    button("Start", 12, 296, ACCENT, INK, setupNextCb);
  } else if (setupStep == 1) {
    lv_obj_t* h = label(s, "How other people see you on the mesh.", &font_tdos_12, MUTE);
    lv_obj_set_pos(h, 16, 76);
    taInput = lv_textarea_create(s);
    lv_obj_set_size(taInput, 296, 44);
    lv_obj_set_pos(taInput, 12, 104);
    lv_textarea_set_one_line(taInput, true);
    lv_textarea_set_max_length(taInput, 31);
    lv_textarea_set_text(taInput, lastName);
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
    button("Back", 12, 140, RAISE2, TEXT, setupBackCb);
    button("Next", 168, 140, ACCENT, INK, setupNextCb);
  } else if (setupStep == 2) {
    char off[16], b[48];
    fmtOffset(cfg().tzMinutes, off, sizeof(off));
    int z = zoneIndex();
    lv_obj_t* zl = label(s, z >= 0 ? zones[z].name : off, &font_tdos_20, ACCENT);
    lv_obj_set_pos(zl, 16, 68);
    lv_obj_t* ol = label(s, off, &font_tdos_12, MUTE);
    lv_obj_set_pos(ol, 16, 94);
    lv_obj_t* zb = tappable(box(s, 144, 40, RAISE2, 12));
    lv_obj_set_pos(zb, 12, 114);
    lv_obj_center(label(zb, "Change zone", &font_tdos_14, TEXT));
    lv_obj_add_event_cb(zb, setupZoneCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* db = tappable(box(s, 144, 40, RAISE2, 12));
    lv_obj_set_pos(db, 168, 114);
    snprintf(b, sizeof(b), "Daylight saving: %s", cfg().dst ? "On" : "Off");
    lv_obj_center(label(db, b, &font_tdos_12, TEXT));
    lv_obj_add_event_cb(db, setupDstCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* hl = label(s, "The clock shows local time once GPS or the mesh provides the time.", &font_tdos_12, MUTE);
    lv_obj_set_width(hl, 296);
    lv_obj_set_pos(hl, 16, 160);
    button("Back", 12, 140, RAISE2, TEXT, setupBackCb);
    button("Next", 168, 140, ACCENT, INK, setupNextCb);
  } else {
    lv_obj_t* l = label(s, "\xE2\x80\xA2 Swipe in from the left edge: back\n\xE2\x80\xA2 Swipe up from the bottom edge: Home\n\xE2\x80\xA2 Swipe down from the top: quick menu\n\xE2\x80\xA2 Settings has GPS, sounds and radio", &font_tdos_14, TEXT);
    lv_obj_set_width(l, 296);
    lv_obj_set_pos(l, 16, 80);
    button("Get started", 12, 296, ACCENT, INK, setupNextCb);
  }
  loadApp(s);
}

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
  addRow("GPS", c.gpsEnabled ? "On" : "Off", ROW_GPS, K_VALUE, c.gpsEnabled);
  snprintf(b, sizeof(b), "%d%%", c.brightness * 100 / 16);
  addRow("Brightness", b, ROW_BRIGHT, K_NAV);
  if (c.volume == 0) snprintf(b, sizeof(b), "Muted");
  else snprintf(b, sizeof(b), "%d%%", c.volume);
  addRow("Volume", b, ROW_VOLUME, K_NAV);
  static const char* const chNames[3] = { "Off", "Mentions only", "All messages" };
  addRow("Channel sounds", chNames[constrain(c.channelSound, 0, 2)], ROW_CHSOUND, K_NAV);
  addRow("Screen timeout", TIMEOUT_NAMES[constrain((int)c.screenTimeout, 0, TIMEOUT_N - 1)], ROW_TIMEOUT, K_NAV);
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
  addRow("Run setup again", "Start", AD_SETUP, K_BUTTON);
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
    case ROW_GPS:                              // main.cpp's GPS policy reads this every pass
      c.gpsEnabled = c.gpsEnabled ? 0 : 1;
      break;
    case ROW_BRIGHT: openLevel(0); return;
    case ROW_VOLUME: openLevel(1); return;
    case ROW_CHSOUND:
      snprintf(pickBuf[0], sizeof(pickBuf[0]), "Off");
      snprintf(pickBuf[1], sizeof(pickBuf[1]), "Mentions only");
      snprintf(pickBuf[2], sizeof(pickBuf[2]), "All messages");
      openPicker("Channel sounds", 3, constrain(c.channelSound, 0, 2), pickChannelSound, M_SETTINGS);
      return;
    case ROW_TIMEOUT:
      for (int i = 0; i < TIMEOUT_N; i++) snprintf(pickBuf[i], sizeof(pickBuf[i]), "%s", TIMEOUT_NAMES[i]);
      openPicker("Screen timeout", TIMEOUT_N, constrain((int)c.screenTimeout, 0, TIMEOUT_N - 1), pickTimeout, M_SETTINGS);
      return;
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
    case AD_SETUP:                       // e.g. when handing the device to someone else
      cfg().setupDone = 0;
      settings_changed();
      openSetup(0);
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
    lv_obj_t* t = label(nodesBox, "Nobody heard yet.\nNodes appear as they advertise.", &font_tdos_14, MUTE);
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
    if (ni->rssi) snprintf(sig, sizeof(sig), "%d dBm\n%s", ni->rssi, age);
    else snprintf(sig, sizeof(sig), "\n%s", age);
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

  const char* caps[4] = { "REPLY", "HOPS", "RSSI dBm", "SNR dB" };
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

// ── Status: is everything healthy ────────────────────────────────
static void fmtDuration(uint32_t s, char* out, size_t n) {
  if (s < 60) snprintf(out, n, "%us", (unsigned)s);
  else if (s < 3600) snprintf(out, n, "%um %us", (unsigned)(s / 60), (unsigned)(s % 60));
  else if (s < 86400) snprintf(out, n, "%uh %um", (unsigned)(s / 3600), (unsigned)((s % 3600) / 60));
  else snprintf(out, n, "%ud %uh", (unsigned)(s / 86400), (unsigned)((s % 86400) / 3600));
}

static void refreshStatus() {
  if (!stPill) return;
  const UiModel& m = stLast;
  char b[48], c[48];

  snprintf(b, sizeof(b), "%d%%", m.batteryPct);
  snprintf(c, sizeof(c), "%.2f V%s", m.batteryMv / 1000.0f, m.charging ? ", charging" : "");
  lv_label_set_text(stVal[0], b); lv_label_set_text(stSub[0], c);

  fmtDuration(m.uptimeSec, b, sizeof(b));
  lv_label_set_text(stVal[1], b); lv_label_set_text(stSub[1], "since power on");

  int repeaters = 0, n = nodes_count();
  for (int i = 0; i < n; i++) { const NodeInfo* ni = nodes_sorted(i); if (ni && ni->type == 2) repeaters++; }
  snprintf(b, sizeof(b), "%d nodes", n);
  snprintf(c, sizeof(c), "%d repeaters", repeaters);
  lv_label_set_text(stVal[2], b); lv_label_set_text(stSub[2], c);

  snprintf(b, sizeof(b), "%lu / %lu", (unsigned long)m.packetsRx, (unsigned long)m.packetsTx);
  uint32_t pct10 = m.uptimeSec ? (uint32_t)((uint64_t)m.airtimeMs * 1000ULL / ((uint64_t)m.uptimeSec * 1000ULL)) : 0;   // tenths of a percent
  snprintf(c, sizeof(c), "in / out, %lu.%lu%% airtime", (unsigned long)(pct10 / 10), (unsigned long)(pct10 % 10));
  lv_label_set_text(stVal[3], b); lv_label_set_text(stSub[3], c);

  const Settings& cs = cfg();
  snprintf(b, sizeof(b), "%.3f MHz", cs.freq);
  snprintf(c, sizeof(c), "SF%d  BW %.4g  4/%d", cs.sf, cs.bw, cs.cr);
  lv_label_set_text(stVal[4], b); lv_label_set_text(stSub[4], c);

  if (m.sdReady) {
    snprintf(b, sizeof(b), "%.1f GB free", m.sdFreeMb / 1024.0f);
    lv_label_set_text(stVal[5], "SD card");
    lv_label_set_text(stSub[5], b);
  } else {
    lv_label_set_text(stVal[5], "No SD card");
    lv_label_set_text(stSub[5], "history is not saved");
  }

  // GPS section
  const char* fix = m.gpsState == 3 ? "GPS off" : m.gpsState != 2 ? (m.gpsState == 1 ? "Searching" : "No data")
                  : m.gpsFixType >= 3 ? "3D fix" : "2D fix";
  if (m.gpsWindowSec > 0) snprintf(c, sizeof(c), "start-up window, %us left", (unsigned)m.gpsWindowSec);
  else if (m.gpsState == 3) snprintf(c, sizeof(c), "switched off in Settings");
  else if (m.gpsState == 2) snprintf(c, sizeof(c), "updated %us ago", (unsigned)m.gpsFixAgeSec);
  else snprintf(c, sizeof(c), m.gpsState == 1 ? "looking for satellites" : "module is silent");
  lv_label_set_text(stVal[6], fix); lv_label_set_text(stSub[6], c);
  lv_obj_set_style_text_color(stVal[6], C(m.gpsState == 2 ? GOOD : m.gpsState == 1 ? ACCENT : MUTE), 0);

  snprintf(b, sizeof(b), "%u used", (unsigned)m.gpsSatsUsed);
  snprintf(c, sizeof(c), "%u in view", (unsigned)m.gpsSatsView);
  lv_label_set_text(stVal[7], b); lv_label_set_text(stSub[7], c);

  // 99.99 and 100 are the module's "no estimate" values, not measurements
  if (m.gpsState == 2 && m.gpsHdop > 0 && m.gpsHdop < 50) {
    snprintf(b, sizeof(b), "HDOP %.1f", m.gpsHdop);
    const char* q = m.gpsHdop < 1.0f ? "excellent" : m.gpsHdop < 2.0f ? "good" : m.gpsHdop < 5.0f ? "moderate" : "poor";
    snprintf(c, sizeof(c), "%s, about %d m", q, (int)lroundf(m.gpsHdop * 5.0f));
  } else {
    snprintf(b, sizeof(b), "-"); snprintf(c, sizeof(c), "no estimate yet");
  }
  lv_label_set_text(stVal[8], b); lv_label_set_text(stSub[8], c);

  // Vertical error is typically two to three times the horizontal error, and a 2D fix has none.
  if (m.gpsState == 2 && m.gpsFixType >= 3) {
    snprintf(b, sizeof(b), "%.0f m", m.gpsAltM);
    snprintf(c, sizeof(c), "%.0f ft, rough", m.gpsAltM * 3.28084f);
  } else { snprintf(b, sizeof(b), "-"); c[0] = 0; }
  lv_label_set_text(stVal[9], b); lv_label_set_text(stSub[9], c);

  // Below about 4 km/h a GPS cannot tell walking from sitting still, so call that stationary.
  // And with poor geometry (HDOP 5 or more) the figure is not worth showing at all.
  if (m.gpsState == 2 && m.gpsFixType >= 3 && m.gpsHdop > 0 && m.gpsHdop < 5.0f) {
    if (m.gpsSpeedKmh >= 4.0f) {
      snprintf(b, sizeof(b), "%.0f km/h", m.gpsSpeedKmh);
      snprintf(c, sizeof(c), "heading %d deg", (int)lroundf(m.gpsCourse));
    } else {
      snprintf(b, sizeof(b), "Stationary");
      c[0] = 0;
    }
  } else { snprintf(b, sizeof(b), "-"); snprintf(c, sizeof(c), m.gpsState == 2 ? "geometry too poor" : ""); }
  lv_label_set_text(stVal[10], b); lv_label_set_text(stSub[10], c);

  if (m.hasPos) {
    double la = m.latE6 / 1e6, lo = m.lonE6 / 1e6;
    snprintf(b, sizeof(b), "%.4f %c", fabs(la), la >= 0 ? 'N' : 'S');
    snprintf(c, sizeof(c), "%.4f %c", fabs(lo), lo >= 0 ? 'E' : 'W');
  } else { snprintf(b, sizeof(b), "-"); c[0] = 0; }
  lv_label_set_text(stVal[11], b); lv_label_set_text(stSub[11], c);

  // Healthy means: no long stalls, memory not running out, and the radio is working.
  bool stalls = m.loopWorstMs > 400;
  bool lowMem = m.heapKb < 40;
  bool errs = m.packetsRx > 20 && m.rxErrors * 4 > m.packetsRx;
  bool ok = !stalls && !lowMem && !errs;
  lv_label_set_text(stPillLbl, ok ? "Healthy" : "Check");
  lv_obj_set_style_bg_color(stPill, C(ok ? GOOD : ACCENT), 0);
  const char* ts = m.timeSource == 2 ? "GPS" : m.timeSource == 1 ? "mesh" : "no clock";
  snprintf(b, sizeof(b), "loop %lu ms, %lu KB, %s", (unsigned long)m.loopWorstMs, (unsigned long)m.heapKb, ts);
  lv_label_set_text(stNote, b);
}

// A scrolling page: the device's health first, then what the GPS module reports.
static void openStatus() {
  mode = M_STATUS;
  lv_obj_t* s = newScreen();
  headerFor(s, "Status");
  stBox = lv_obj_create(s);
  lv_obj_set_size(stBox, 320, 240 - 58);
  lv_obj_set_pos(stBox, 0, 58);
  lv_obj_set_style_bg_opa(stBox, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(stBox, 0, 0);
  lv_obj_set_style_pad_all(stBox, 0, 0);
  lv_obj_set_scrollbar_mode(stBox, LV_SCROLLBAR_MODE_OFF);

  const char* titles[12] = { "BATTERY", "UPTIME", "HEARD", "PACKETS", "RADIO", "STORAGE",
                             "GPS FIX", "SATELLITES", "ACCURACY", "ALTITUDE", "SPEED", "POSITION" };
  for (int i = 0; i < 12; i++) {
    int row = i / 2, col = i % 2;
    int y = 4 + row * 50 + (i >= 6 ? 54 : 0);            // a gap after the health tiles for the strip
    lv_obj_t* t = box(stBox, 154, 46, RAISE, 12);
    lv_obj_set_pos(t, 4 + col * 158, y);
    lv_obj_t* cap = label(t, titles[i], &font_tdos_12, MUTE);
    lv_obj_set_pos(cap, 10, 3);
    stVal[i] = label(t, "-", &font_tdos_14, TEXT);
    lv_obj_set_pos(stVal[i], 10, 17);
    lv_obj_set_width(stVal[i], 136);
    lv_label_set_long_mode(stVal[i], LV_LABEL_LONG_CLIP);
    stSub[i] = label(t, "", &font_tdos_12, MUTE);
    lv_obj_set_pos(stSub[i], 10, 31);
    lv_obj_set_width(stSub[i], 136);
    lv_label_set_long_mode(stSub[i], LV_LABEL_LONG_CLIP);
  }
  stPill = box(stBox, 76, 22, GOOD, 11);
  lv_obj_set_pos(stPill, 6, 158);
  stPillLbl = label(stPill, "Healthy", &font_tdos_12, INK);
  lv_obj_center(stPillLbl);
  stNote = label(stBox, "", &font_tdos_12, MUTE);
  lv_obj_set_pos(stNote, 90, 161);
  lv_obj_set_width(stNote, 226);
  lv_label_set_long_mode(stNote, LV_LABEL_LONG_CLIP);
  lv_obj_t* gps = label(stBox, "GPS", &font_tdos_12, ACCENT);
  lv_obj_set_pos(gps, 10, 188);
  refreshStatus();
  loadApp(s);
}

// ── Quick panel: swipe down from the top edge ────────────────────
static lv_obj_t *quickBackdrop = nullptr;
static bool quickOpen = false;
static lv_obj_t *quickGpsIcon = nullptr, *quickGpsText = nullptr;

static void closeQuick(bool immediate = false) {
  if (!quickOpen) return;
  quickOpen = false;
  lv_obj_t* b = quickBackdrop;
  quickBackdrop = nullptr;
  if (!b) return;
  if (immediate) lv_obj_del(b);
  else lv_obj_del_async(b);               // closing from a tap on one of its own buttons
}
static void quickBackdropCb(lv_event_t*) { closeQuick(); }
static void quickBrightCb(lv_event_t* e) {
  int v = constrain(lv_slider_get_value(lv_event_get_target(e)), 1, 16);
  cfg().brightness = (uint8_t)v;
  setBacklight(v);
  settings_changed();
}
static void quickVolumeCb(lv_event_t* e) {
  int v = constrain(lv_slider_get_value(lv_event_get_target(e)), 0, 10);
  cfg().volume = (uint8_t)(v * 10);
  audio_set_volume(cfg().volume);
  settings_changed();
}
static void quickVolumeReleaseCb(lv_event_t*) { audio_play(SND_BLIP); }
static void quickAdvertCb(lv_event_t*) {
  if (actions.advertise) actions.advertise();
  closeQuick();
  toast("Advert sent");
}
// Toggles in place: the panel stays open so the change is visible, and main.cpp acts on the setting.
static void quickGpsCb(lv_event_t*) {
  cfg().gpsEnabled = cfg().gpsEnabled ? 0 : 1;
  settings_changed();
  if (quickGpsIcon) lv_obj_set_style_text_color(quickGpsIcon, C(cfg().gpsEnabled ? GOOD : 0x3A4658), 0);
  if (quickGpsText) lv_label_set_text(quickGpsText, cfg().gpsEnabled ? "GPS on" : "GPS off");
}
static void quickLockCb(lv_event_t*) { closeQuick(); pendingGesture = G_NONE; goLock(); }
static void quickSettingsCb(lv_event_t*) { closeQuick(); openSettings(); }

static void openQuick() {
  if (quickOpen || mode == M_LOCK || mode == M_SETUP) return;
  quickOpen = true;
  // The backdrop dims the screen and catches any tap outside the panel as "close".
  quickBackdrop = tappable(box(lv_layer_top(), 320, 240, 0x000000, 0));
  lv_obj_set_style_bg_opa(quickBackdrop, LV_OPA_50, 0);
  lv_obj_set_pos(quickBackdrop, 0, 0);
  lv_obj_add_event_cb(quickBackdrop, quickBackdropCb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* panel = box(quickBackdrop, 320, 156, RAISE, 0);
  lv_obj_set_pos(panel, 0, 22);
  lv_obj_t* title = label(panel, "Quick settings", &font_tdos_12, MUTE);
  lv_obj_set_pos(title, 12, 8);

  lv_obj_t* sun = label(panel, LV_SYMBOL_EYE_OPEN, &font_tdos_14, TEXT);
  lv_obj_set_pos(sun, 12, 32);
  lv_obj_t* sl = lv_slider_create(panel);
  lv_obj_set_size(sl, 250, 14);
  lv_obj_set_pos(sl, 46, 36);
  lv_slider_set_range(sl, 1, 16);
  lv_slider_set_value(sl, cfg().brightness, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(sl, C(RAISE2), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sl, C(ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, C(TEXT), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_pad_all(sl, 5, LV_PART_KNOB);
  lv_obj_add_event_cb(sl, quickBrightCb, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t* spk = label(panel, LV_SYMBOL_VOLUME_MAX, &font_tdos_14, TEXT);
  lv_obj_set_pos(spk, 12, 60);
  lv_obj_t* vs = lv_slider_create(panel);
  lv_obj_set_size(vs, 250, 14);
  lv_obj_set_pos(vs, 46, 64);
  lv_slider_set_range(vs, 0, 10);
  lv_slider_set_value(vs, cfg().volume / 10, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(vs, C(RAISE2), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(vs, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(vs, C(ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(vs, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(vs, C(TEXT), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(vs, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_pad_all(vs, 5, LV_PART_KNOB);
  lv_obj_add_event_cb(vs, quickVolumeCb, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(vs, quickVolumeReleaseCb, LV_EVENT_RELEASED, nullptr);

  struct Btn { const char* text; const char* sym; lv_event_cb_t cb; };
  const Btn btns[4] = {
    { "Advert", LV_SYMBOL_UPLOAD, quickAdvertCb },
    { "Lock", LV_SYMBOL_POWER, quickLockCb },
    { cfg().gpsEnabled ? "GPS on" : "GPS off", LV_SYMBOL_GPS, quickGpsCb },
    { "Settings", LV_SYMBOL_SETTINGS, quickSettingsCb },
  };
  for (int i = 0; i < 4; i++) {
    lv_obj_t* b = tappable(box(panel, 74, 52, RAISE2, 14));
    lv_obj_set_pos(b, 6 + i * 78, 92);
    bool isGps = (i == 2);
    uint32_t iconCol = isGps ? (cfg().gpsEnabled ? GOOD : 0x3A4658) : ACCENT;
    lv_obj_t* ic = label(b, btns[i].sym, &font_tdos_16, iconCol);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_t* tx = label(b, btns[i].text, &font_tdos_12, TEXT);
    lv_obj_align(tx, LV_ALIGN_BOTTOM_MID, 0, -6);
    if (isGps) { quickGpsIcon = ic; quickGpsText = tx; }
    lv_obj_add_event_cb(b, btns[i].cb, LV_EVENT_CLICKED, nullptr);
  }

  // Slide down from above the screen.
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, panel);
  lv_anim_set_values(&a, -156, 22);
  lv_anim_set_time(&a, 160);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

// ── Map ──────────────────────────────────────────────────────────
static double pendingFocusLat = 0, pendingFocusLon = 0;
static char pendingFocusName[40];
static bool pendingFocus = false;
static void openMap() {
  mapReturnConv = -1;
  mode = M_MAP;
  lv_obj_t* s = newScreen();
  mapview_create(s);
  // a small back button over the map (the tiles are the whole screen)
  lv_obj_t* back = tappable(box(s, 36, 28, RAISE, 10));
  lv_obj_set_style_bg_opa(back, LV_OPA_80, 0);
  lv_obj_set_pos(back, 6, 28);
  lv_obj_center(label(back, LV_SYMBOL_LEFT, &font_tdos_14, TEXT));
  lv_obj_add_event_cb(back, backCb, LV_EVENT_CLICKED, nullptr);
  if (pendingFocus) {
    pendingFocus = false;
    mapview_focus(pendingFocusLat, pendingFocusLon, pendingFocusName);
  }
  loadApp(s);
}

// The same screen, opened on a particular place. Back returns to the conversation it came from.
static void openMapFocus(double lat, double lon, const char* name) {
  int keep = mapReturnConv;
  pendingFocus = true;
  pendingFocusLat = lat; pendingFocusLon = lon;
  snprintf(pendingFocusName, sizeof(pendingFocusName), "%s", name);
  openMap();
  mapReturnConv = keep;                      // openMap() clears it for the ordinary case
}

// ── Message details: how a message travelled (TDOS-002) ──────────
static void formatClockSec(uint32_t ts, char* out, size_t n) {
  out[0] = 0;
  if (ts < 1700000000UL) return;
  struct tm tm;
  localTime(ts, tm);
  int h12 = tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12;
  snprintf(out, n, "%d:%02d:%02d %s", h12, tm.tm_min, tm.tm_sec, tm.tm_hour < 12 ? "AM" : "PM");
}

// Name a path hash from the repeaters we know. One byte only has 256 values, so this is often
// ambiguous: say so rather than guess.
static void hopName(const uint8_t* hash, int size, char* out, size_t n) {
  int matches = 0;
  const NodeInfo* hit = nullptr;
  for (int i = 0; i < nodes_count(); i++) {
    const NodeInfo* ni = nodes_sorted(i);
    if (!ni || (ni->type != 2 && ni->type != 3)) continue;
    if (memcmp(ni->key, hash, size > 4 ? 4 : size) == 0) { matches++; if (!hit) hit = ni; }
  }
  char hex[10] = "";
  for (int k = 0; k < size && k < 3; k++) snprintf(hex + strlen(hex), sizeof(hex) - strlen(hex), "%02X", hash[k]);
  if (matches == 1) {
    char nm[40];
    emoji_filter(hit->name, nm, sizeof(nm), false);
    snprintf(out, n, "%s", nm);
  } else if (matches > 1) {
    snprintf(out, n, "%s (%d match)", hex, matches);
  } else {
    snprintf(out, n, "%s (unknown)", hex);
  }
}

static void openMessageDetails(int conv, int idx) {
  const Conv* c = msgs_conv(conv);
  const Msg* m = msgs_at(conv, idx);
  if (!c || !m) return;
  mode = M_DETAIL;
  detailConv = conv;
  detailIdx = idx;
  lv_obj_t* s = newScreen();
  headerFor(s, "Message");
  newListBox(s);
  listSel = 0;

  char b[64], t[24];
  char who[48];
  emoji_filter(m->out ? c->name : (m->sender[0] ? m->sender : c->name), who, sizeof(who), false);
  addRow(m->out ? "To" : "From", who, 0, K_INFO);

  if (m->out) {
    formatClockSec(m->ts, t, sizeof(t));
    if (t[0]) addRow("Sent", t, 1, K_INFO);
    const char* st = m->status == MS_DELIVERED ? "Delivered" : m->status == MS_SENT ? "Sent, no reply yet"
                   : m->status == MS_FAILED ? "Not delivered" : "Sending";
    addRow("Status", st, 2, K_INFO);
    if (m->rtt100) { snprintf(b, sizeof(b), "%.1f s", m->rtt100 / 10.0f); addRow("Round trip", b, 3, K_INFO); }
    snprintf(b, sizeof(b), "%u characters", (unsigned)strlen(m->text));
    addRow("Length", b, 4, K_INFO);
  } else {
    formatClockSec(m->ts, t, sizeof(t));
    if (t[0]) addRow("Received", t, 1, K_INFO);
    if (m->d.senderTs) {
      char st[24];
      formatClockSec(m->d.senderTs, st, sizeof(st));
      if (st[0]) addRow("Sent (their clock)", st, 2, K_INFO);
      if (m->ts >= 1700000000UL) {
        long diff = (long)m->ts - (long)m->d.senderTs;
        long ad = diff < 0 ? -diff : diff;
        if (ad < 120) snprintf(b, sizeof(b), "%+ld s", diff);
        else if (ad < 7200) snprintf(b, sizeof(b), "%+ld min", diff / 60);
        else snprintf(b, sizeof(b), "%+ld h", diff / 3600);
        if (ad > 600) strncat(b, ", clock looks wrong", sizeof(b) - strlen(b) - 1);
        addRow("Clock difference", b, 3, K_INFO);
      }
    }
    if (m->flags & MF_RADIO) {
      if (!(m->flags & MF_FLOOD)) snprintf(b, sizeof(b), "Routed to us");
      else if (m->hops == 0) snprintf(b, sizeof(b), "Heard directly");
      else snprintf(b, sizeof(b), "Flood, %d hop%s", m->hops, m->hops == 1 ? "" : "s");
      addRow("Route", b, 4, K_INFO);
      char sn[16];
      formatSnr(m->snr4, sn, sizeof(sn));
      snprintf(b, sizeof(b), "SNR %s, RSSI %d", sn, m->rssi);
      addRow("Last hop", b, 5, K_INFO);
    }
    if (m->d.hash4[0] || m->d.hash4[1] || m->d.hash4[2] || m->d.hash4[3]) {
      uint8_t extra = tables.extraCopies(m->d.hash4);
      if (extra == 0) snprintf(b, sizeof(b), "once");
      else snprintf(b, sizeof(b), "%u times", (unsigned)extra + 1);
      addRow("Heard", b, 6, K_INFO);
    }
    if (m->d.payloadLen) {
      snprintf(b, sizeof(b), "%u bytes, about %u ms", (unsigned)m->d.payloadLen, (unsigned)m->d.airMs);
      addRow("Size and airtime", b, 7, K_INFO);
    }
    if (m->d.pathCount > 0 && m->d.pathSize >= 1 && m->d.pathSize <= 3) {
      snprintf(b, sizeof(b), "%u repeater%s%s", (unsigned)m->d.pathCount, m->d.pathCount == 1 ? "" : "s",
               m->hops > m->d.pathCount ? " (first of more)" : "");
      addRow("Path", b, 8, K_INFO);
      for (int h = 0; h < m->d.pathCount && listN < 20; h++) {
        char hop[48], title[12];
        hopName(m->d.path + h * m->d.pathSize, m->d.pathSize, hop, sizeof(hop));
        snprintf(title, sizeof(title), "  Hop %d", h + 1);
        addRow(title, hop, 20 + h, K_INFO);
      }
    }
    int32_t la, lo;
    char nm[40];
    if (messagePosition(c, m, &la, &lo, nm, sizeof(nm))) addRow("Where it came from", "Show on map", 100, K_BUTTON);
  }
  listHighlight();
  loadApp(s);
}

static void detailAct(int id) {
  if (id != 100) return;
  const Conv* c = msgs_conv(detailConv);
  const Msg* m = msgs_at(detailConv, detailIdx);
  int32_t la, lo;
  char who[40], shown[48];
  if (!c || !m || !messagePosition(c, m, &la, &lo, who, sizeof(who))) return;
  mapReturnConv = detailConv;
  emoji_filter(who, shown, sizeof(shown), false);
  openMapFocus(la / 1e6, lo / 1e6, shown);
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
  // GPS: grey with a cross while the module is silent, amber and pulsing while it searches,
  // solid green once it has a position.
  barGps = label(bar, LV_SYMBOL_GPS, &font_tdos_12, MUTE);
  lv_obj_align_to(barGps, barSig, LV_ALIGN_OUT_LEFT_MID, -10, 0);
  barGpsX = label(bar, LV_SYMBOL_CLOSE, &font_tdos_12, BAD);
  lv_obj_align_to(barGpsX, barGps, LV_ALIGN_CENTER, 0, 0);
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

// ── touch read callback, with edge swipes ────────────────────────
// A touch that starts in an edge zone is held back from LVGL until it is clear what it is:
//   left edge, swipe in     back          bottom edge, swipe up   home
//   top edge, swipe down    quick panel
// If the finger is lifted without a swipe it was an ordinary tap that happened near an edge, and
// it is replayed to LVGL as a press and release at the point where it began.
static constexpr int EDGE_LEFT = 20, EDGE_BOTTOM = 20, EDGE_TOP = 18;    // zone sizes in pixels
static constexpr int SWIPE_MIN = 36;                                     // travel needed to count

static void touchRead(lv_indev_drv_t*, lv_indev_data_t* data) {
  static int16_t lastX = 0, lastY = 0;        // most recent position, reported to LVGL
  static bool down = false, edge = false, fired = false;
  static int16_t sx = 0, sy = 0;              // where the current touch began
  static int edgeKind = 0;                    // 1 left, 2 bottom, 3 top
  static uint8_t tapPhase = 0;                // replaying a tap: 1 = press, 2 = release
  static int16_t tapX = 0, tapY = 0;

  bool have = false, nowDown = false;
  if (touchAddr) {
    uint8_t s;
    if (gt911Read(0x814E, &s, 1) && (s & 0x80)) {
      have = true;
      uint8_t n = s & 0x0F;
      if (n > 0) {
        uint8_t p[4];
        if (gt911Read(0x8150, p, 4)) {
          int px = p[0] | (p[1] << 8), py = p[2] | (p[3] << 8);
          lastX = (int16_t)constrain(py, 0, 319);        // portrait panel -> landscape (rotation 1)
          lastY = (int16_t)constrain(239 - px, 0, 239);
          nowDown = true;
        }
      }
      gt911Clear();
    }
  }

  if (have && nowDown) {
    lastActivityMs = millis();
    if (!down) {                                          // a new touch begins
      down = true;
      sx = lastX; sy = lastY;
      fired = false;
      edgeKind = 0;
      if (mode == M_LOCK) { wake(); touchHeld = true; }
      else if (lastX <= EDGE_LEFT) edgeKind = 1;
      else if (lastY >= 240 - EDGE_BOTTOM) edgeKind = 2;
      else if (lastY <= EDGE_TOP) edgeKind = 3;
      edge = edgeKind != 0;
    } else if (edge && !fired) {                          // moving: has it become a swipe?
      int dx = lastX - sx, dy = lastY - sy;
      if (edgeKind == 1 && dx >= SWIPE_MIN && abs(dy) < abs(dx)) { pendingGesture = G_BACK; fired = true; }
      else if (edgeKind == 2 && dy <= -SWIPE_MIN && abs(dx) < abs(dy)) { pendingGesture = G_HOME; fired = true; }
      else if (edgeKind == 3 && dy >= SWIPE_MIN && abs(dx) < abs(dy)) { pendingGesture = G_QUICK; fired = true; }
    }
  } else if (have && !nowDown && down) {                  // the finger was lifted
    down = false;
    touchHeld = false;
    if (edge && !fired) { tapPhase = 1; tapX = sx; tapY = sy; }   // just a tap near an edge
    edge = false;
  }

  if (tapPhase == 1) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = tapX; data->point.y = tapY;
    tapPhase = 2;
    return;
  }
  if (tapPhase == 2) {
    data->state = LV_INDEV_STATE_RELEASED;
    data->point.x = tapX; data->point.y = tapY;
    tapPhase = 0;
    return;
  }
  data->point.x = lastX;
  data->point.y = lastY;
  data->state = (down && !edge && !touchHeld) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
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

static void runGesture() {
  Gesture g = pendingGesture;
  pendingGesture = G_NONE;
  if (g == G_NONE || mode == M_LOCK || mode == M_SETUP) return;
  ui_activity();
  if (quickOpen) { closeQuick(true); return; }           // any edge swipe first dismisses the panel
  if (g == G_QUICK) { openQuick(); return; }
  if (g == G_HOME) { if (mode != M_HOME) showHome(); return; }
  if (g == G_BACK) { if (mode != M_HOME) goBack(); return; }
}

void ui_tick() {
  lv_timer_handler();
  runGesture();
  mapview_tick();
  uint32_t idleS = TIMEOUT_SECONDS[constrain((int)cfg().screenTimeout, 0, TIMEOUT_N - 1)];
  if (idleS && mode != M_LOCK && millis() - lastActivityMs > idleS * 1000UL) goLock();
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
  {
    bool pulse = (millis() / 500) % 2;
    uint32_t col = m.gpsState == 2 ? GOOD : m.gpsState == 1 ? (pulse ? ACCENT : MUTE) : m.gpsState == 3 ? 0x3A4658 : MUTE;
    lv_obj_set_style_text_color(barGps, C(col), 0);
    lv_obj_align_to(barGps, barSig, LV_ALIGN_OUT_LEFT_MID, -10, 0);
    lv_obj_align_to(barGpsX, barGps, LV_ALIGN_CENTER, 0, 0);
    if (m.gpsState == 0) lv_obj_clear_flag(barGpsX, LV_OBJ_FLAG_HIDDEN);       // only "module silent" gets the cross
    else lv_obj_add_flag(barGpsX, LV_OBJ_FLAG_HIDDEN);
  }

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
  stLast.batteryPct = m.batteryPct; stLast.charging = m.charging; stLast.batteryMv = m.batteryMv;
  stLast.uptimeSec = m.uptimeSec; stLast.packetsRx = m.packetsRx; stLast.packetsTx = m.packetsTx;
  stLast.rxErrors = m.rxErrors; stLast.airtimeMs = m.airtimeMs; stLast.loopWorstMs = m.loopWorstMs;
  stLast.heapKb = m.heapKb; stLast.psramKb = m.psramKb; stLast.timeSource = m.timeSource; stLast.sdReady = m.sdReady; stLast.sdFreeMb = m.sdFreeMb;
  stLast.gpsState = m.gpsState; stLast.gpsFixType = m.gpsFixType; stLast.gpsSatsUsed = m.gpsSatsUsed;
  stLast.gpsSatsView = m.gpsSatsView; stLast.gpsHdop = m.gpsHdop; stLast.gpsAltM = m.gpsAltM;
  stLast.gpsSpeedKmh = m.gpsSpeedKmh; stLast.gpsCourse = m.gpsCourse; stLast.gpsFixAgeSec = m.gpsFixAgeSec;
  stLast.hasPos = m.hasPos; stLast.latE6 = m.latE6; stLast.lonE6 = m.lonE6;
  if (mode == M_STATUS) refreshStatus();
  mapview_update(m);
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
  } else if (mode == M_STATUS && dy && stBox) {
    lv_obj_scroll_by(stBox, 0, -dy * 50, LV_ANIM_OFF);
  } else if (mode == M_MAP) {
    mapview_pan(dx * 48, dy * 48);
  } else if (mode == M_DETAIL && dy && listBox) {
    lv_obj_scroll_by(listBox, 0, -dy * 38, LV_ANIM_OFF);
  } else if (mode == M_NODES && dy && nodesBox) {
    lv_obj_scroll_by(nodesBox, 0, -dy * 50, LV_ANIM_OFF);
  } else if (mode == M_BRIGHT && dx && brightSlider) {
    int v = constrain(lv_slider_get_value(brightSlider) + dx, levelKind == 0 ? 1 : 0, levelKind == 0 ? 16 : 10);
    lv_slider_set_value(brightSlider, v, LV_ANIM_OFF);
    brightApply(v);
  }
}

void ui_click() {
  if (quickOpen) { ui_activity(); closeQuick(true); return; }
  if (mode == M_LOCK) { ui_activity(); wake(); return; }
  ui_activity();
  if (mode == M_HOME) openApp(sel);
  else if (mode == M_CONVS && convN > 0) openThread(convOrder[convSel]);
  else if (mode == M_THREAD) doSend();
  else if (mode == M_ENTRY) entrySave();
  else if (mode == M_SETUP) setupNext();
  else if (mode == M_SIGNAL) sigSend();
  else if (mode == M_MAP) mapview_centre_on_me();
  else if ((mode == M_SETTINGS || mode == M_ADV || mode == M_PICK) && listN > 0) {
    if (mode == M_SETTINGS) settingsAct(listIds[listSel]);
    else if (mode == M_ADV) advAct(listIds[listSel]);
    else pickChoose(listIds[listSel]);
  }
}

void ui_long() {
  ui_activity();
  if (mode == M_LOCK) { wake(); return; }
  if (mode == M_SETUP) return;
  showHome();
}

void ui_key(char c) {
  ui_activity();
  if (quickOpen) { closeQuick(true); return; }
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
  if (mode == M_MAP) {
    if (c == '+' || c == '=') { mapview_zoom(1); return; }
    if (c == '-' || c == '_') { mapview_zoom(-1); return; }
    if (c == 'c' || c == 'C') { mapview_centre_on_me(); return; }
    if (c == 8 || c == 127) { goBack(); return; }
    return;
  }
  if (mode == M_SETUP) {
    if (c == '\r' || c == '\n') { setupNext(); return; }
    if (setupStep == 1 && taInput) {
      if (c == 8 || c == 127) lv_textarea_del_char(taInput);
      else if (c >= 32 && c < 127) lv_textarea_add_char(taInput, (uint32_t)c);
    }
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
  if ((mode == M_APP || mode == M_CONVS || mode == M_DBG || mode == M_SETTINGS || mode == M_ADV || mode == M_PICK || mode == M_BRIGHT || mode == M_SIGNAL || mode == M_NODES || mode == M_STATUS || mode == M_DETAIL) && (c == 8 || c == 127)) { goBack(); return; }
}

bool ui_console_active() { return mode == M_DBG; }

uint32_t ui_lvgl_free() {
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  return mon.free_size;
}
