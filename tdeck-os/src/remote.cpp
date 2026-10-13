#include "remote.h"
#ifdef TDOS_REMOTE
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "ui.h"

// Protocol (text lines in, "OK" out; a screenshot is a header line, raw RGB565 little endian, then DONE):
//   tap X Y | swipe X1 Y1 X2 Y2 | key C | keys TEXT | ball DX DY | click | long | info | shot | wait MS
//   key also accepts BS, ENTER, ESC, TAB, SPACE
static constexpr int W = 320, H = 240;
static uint16_t* shadow = nullptr;      // what the display shows, updated by every flush
static uint16_t* snap = nullptr;        // frozen copy being sent

struct Step { int16_t x, y; bool down; };
static Step q[128];
static uint8_t qHead = 0, qTail = 0;
static bool qPush(int x, int y, bool down) {
  uint8_t n = (qTail + 1) % 128;
  if (n == qHead) return false;
  q[qTail] = { (int16_t)x, (int16_t)y, down };
  qTail = n;
  return true;
}

static char line[160];
static int lineLen = 0;
static char reply[96];
static bool replyPending = false;
static bool busy = false;               // a command is still running; its OK follows
static uint32_t busyUntil = 0;          // time-based commands (wait, settling after input)
static bool wantIdle = false;           // OK when the touch queue is empty and the UI had time to react

// screenshot streaming: 0 idle, 1 header, 2 pixels, 3 trailer
static uint8_t txStage = 0;
static size_t txPos = 0;
static char txHead[48];

bool remote_streaming() { return txStage != 0; }

void remote_begin() {
  shadow = (uint16_t*)heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
  snap = (uint16_t*)heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
  if (shadow) memset(shadow, 0, W * H * 2);
}

void remote_capture(const lv_area_t* a, const lv_color_t* c) {
  if (!shadow) return;
  int w = a->x2 - a->x1 + 1;
  for (int y = a->y1; y <= a->y2; y++) {
    if (y < 0 || y >= H || a->x1 < 0 || a->x2 >= W) continue;
    memcpy(&shadow[y * W + a->x1], &c[(y - a->y1) * w], w * 2);
  }
}

bool remote_touch(int16_t& x, int16_t& y, bool& have, bool& down) {
  if (qHead == qTail) return false;
  Step s = q[qHead];
  qHead = (qHead + 1) % 128;
  x = s.x; y = s.y; have = true; down = s.down;
  if (qHead == qTail) busyUntil = millis() + 250;       // give the UI time to act on the last event
  return true;
}

static void say(const char* s) {
  snprintf(reply, sizeof(reply), "%s\n", s);
  replyPending = true;
}

static void tap(int x, int y) {
  for (int i = 0; i < 3; i++) qPush(x, y, true);
  qPush(x, y, false);
  wantIdle = true;
}
static void swipe(int x1, int y1, int x2, int y2) {
  qPush(x1, y1, true); qPush(x1, y1, true);
  for (int i = 1; i <= 6; i++) qPush(x1 + (x2 - x1) * i / 6, y1 + (y2 - y1) * i / 6, true);
  qPush(x2, y2, true);
  qPush(x2, y2, false);
  wantIdle = true;
}

static char keyByName(const char* n) {
  if (!strcmp(n, "BS")) return 8;
  if (!strcmp(n, "ENTER")) return '\r';
  if (!strcmp(n, "ESC")) return 27;
  if (!strcmp(n, "TAB")) return '\t';
  if (!strcmp(n, "SPACE")) return ' ';
  return n[0];
}

static void startShot() {
  if (!shadow || !snap) { say("ERR no buffers"); return; }
  lv_obj_invalidate(lv_scr_act());
  lv_obj_invalidate(lv_layer_top());
  lv_refr_now(NULL);                                    // redraw everything so the shadow is complete
  memcpy(snap, shadow, W * H * 2);
  snprintf(txHead, sizeof(txHead), "SHOT %d %d %d\n", W, H, W * H * 2);
  txStage = 1;
  txPos = 0;
}

static void exec(char* s) {
  int a, b, c, d;
  if (sscanf(s, "tap %d %d", &a, &b) == 2) { tap(a, b); busy = true; return; }
  if (sscanf(s, "swipe %d %d %d %d", &a, &b, &c, &d) == 4) { swipe(a, b, c, d); busy = true; return; }
  if (!strncmp(s, "keys ", 5)) { for (const char* p = s + 5; *p; p++) ui_key(*p); busyUntil = millis() + 200; busy = true; return; }
  if (!strncmp(s, "key ", 4)) { ui_key(keyByName(s + 4)); busyUntil = millis() + 200; busy = true; return; }
  if (sscanf(s, "ball %d %d", &a, &b) == 2) { ui_nav(a, b); busyUntil = millis() + 200; busy = true; return; }
  if (!strcmp(s, "click")) { ui_click(); busyUntil = millis() + 200; busy = true; return; }
  if (!strcmp(s, "long")) { ui_long(); busyUntil = millis() + 300; busy = true; return; }
  if (sscanf(s, "wait %d", &a) == 1) { busyUntil = millis() + a; busy = true; return; }
  if (!strcmp(s, "shot")) { startShot(); busy = true; return; }
  if (!strcmp(s, "info")) {
    char b2[96];
    snprintf(b2, sizeof(b2), "INFO mode=%d lvfree=%u heap=%u", ui_mode(), (unsigned)ui_lvgl_free(), (unsigned)ESP.getFreeHeap());
    say(b2);
    return;
  }
  say("ERR unknown command");
}

void remote_poll() {
  // 1. push out a pending reply or a screenshot, as much as the port takes without blocking
  if (replyPending) {
    int n = strlen(reply);
    if ((int)Serial.availableForWrite() >= n) { Serial.write((const uint8_t*)reply, n); replyPending = false; }
    return;
  }
  if (txStage) {
    int room = Serial.availableForWrite();
    if (room <= 0) return;
    if (txStage == 1) {
      size_t n = strlen(txHead);
      if ((size_t)room < n) return;
      Serial.write((const uint8_t*)txHead, n);
      txStage = 2; txPos = 0;
    } else if (txStage == 2) {
      size_t total = W * H * 2;
      size_t n = min<size_t>(room, total - txPos);
      Serial.write((const uint8_t*)snap + txPos, n);
      txPos += n;
      if (txPos >= total) txStage = 3;
    } else {
      if (room < 6) return;
      Serial.write((const uint8_t*)"\nDONE\n", 6);
      txStage = 0;
      busy = false;
    }
    return;
  }

  // 2. a running command finishes: OK once queued input has been delivered and the UI had time
  if (busy) {
    if (wantIdle && qHead != qTail) return;
    if ((int32_t)(millis() - busyUntil) < 0) return;
    wantIdle = false;
    busy = false;
    say("OK");
    return;
  }

  // 3. read the next command
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      line[lineLen] = 0;
      lineLen = 0;
      if (line[0]) { exec(line); return; }
      continue;
    }
    if (lineLen < (int)sizeof(line) - 1) line[lineLen++] = ch;
  }
}
#endif
