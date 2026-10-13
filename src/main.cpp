// tdeck-os: standalone MeshCore messenger firmware for the LilyGO T-Deck Plus.
//
// This file wires three things together: the hardware (display, touch, keyboard, trackball,
// GPS time, radio), the MeshCore node (DeckMesh below), and the UI (ui.cpp) through a small
// message store (messages.cpp).
//
// Rules carried over from earlier firmware trouble:
//   * the trackball is interrupt-driven; the loop only reads timestamps and counters
//   * serial logging goes through logf(), which can never block on an unread USB port
//   * radio work is non-blocking (MeshCore's loop handles it)
//   * file writes are batched so the loop never stalls on flash

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SPIFFS.h>
#include <TFT_eSPI.h>

#include <Mesh.h>
#include <helpers/ESP32Board.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/IdentityStore.h>
#include <helpers/BaseChatMesh.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <target.h>
#include "ui.h"
#include "messages.h"
#include "settings.h"

#define PUBLIC_GROUP_PSK "izOH6cXN6mrJ5e26oRXNcg=="

// ── Pins (T-Deck Plus) ───────────────────────────────────────────
static constexpr int PIN_POWERON = 10;
static constexpr int PIN_TFT_CS = 12, PIN_SD_CS = 39, PIN_BL = 42;
static constexpr int PIN_I2C_SDA = 18, PIN_I2C_SCL = 8;
static constexpr int PIN_TB_UP = 3, PIN_TB_DOWN = 15, PIN_TB_LEFT = 1, PIN_TB_RIGHT = 2, PIN_TB_CLICK = 0;
static constexpr int PIN_BATT = 4;
static constexpr uint8_t KB_ADDR = 0x55;

// ── Logging that can never block ─────────────────────────────────
// HWCDC::write() waits for the USB host to drain the buffer, and with nobody reading the
// port that wait is unbounded. Only write when the whole line fits.
static void logf(const char* fmt, ...) {
  char b[200];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n > (int)sizeof(b) - 1) n = sizeof(b) - 1;
  if ((int)Serial.availableForWrite() >= n) Serial.write((const uint8_t*)b, n);
}

// ── Trackball: interrupts count edges; the click is timestamped on press and release ──
static volatile uint32_t isrUp, isrDown, isrLeft, isrRight;
static volatile bool clkDown = false, clkReleased = false;
static volatile uint32_t clkDownAt = 0;
static void IRAM_ATTR onUp()    { isrUp++; }
static void IRAM_ATTR onDown()  { isrDown++; }
static void IRAM_ATTR onLeft()  { isrLeft++; }
static void IRAM_ATTR onRight() { isrRight++; }
static void IRAM_ATTR onClick() {
  static uint32_t last;
  uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
  if (now - last < 15) return;     // contact bounce
  last = now;
  if (digitalRead(PIN_TB_CLICK) == LOW) { clkDown = true; clkDownAt = now; }
  else if (clkDown) { clkDown = false; clkReleased = true; }
}

// ── Board ────────────────────────────────────────────────────────
class DeckBoard : public ESP32Board {
public:
  void begin() { startup_reason = BD_STARTUP_NORMAL; }   // skip ESP32Board::begin(): it starts Wire on the wrong pins
  uint16_t getBattMilliVolts() override {
    analogReadResolution(12);
    uint32_t raw = 0;
    for (int i = 0; i < 8; i++) raw += analogRead(PIN_BATT);
    return (uint16_t)((2.0f * 3.3f * 1000.0f * (raw / 8)) / 4096.0f);
  }
  const char* getManufacturerName() const override { return "LilyGo T-Deck"; }
};

// ── Console lines shown on screen ────────────────────────────────
static constexpr int CHAT_LINES = 7;
static char chatBuf[CHAT_LINES][64];
static void chatPush(const char* fmt, ...) {
  for (int i = 0; i < CHAT_LINES - 1; i++) memcpy(chatBuf[i], chatBuf[i + 1], sizeof(chatBuf[0]));
  va_list ap; va_start(ap, fmt);
  vsnprintf(chatBuf[CHAT_LINES - 1], sizeof(chatBuf[0]), fmt, ap);
  va_end(ap);
}

// ── The mesh node ────────────────────────────────────────────────
class DeckMesh : public BaseChatMesh {
  fs::FS* _fs = nullptr;
  struct Pending { uint32_t ack; int conv; uint32_t msgId; uint32_t sentMs; };
  static constexpr int MAX_PENDING = 6;
  Pending _pending[MAX_PENDING] = {};
  int _pubConv = -1;
  ChannelDetails* _public = nullptr;
  bool _dirty = false;
  uint32_t _lastSaveMs = 0;

  void loadContacts() {
    if (!_fs->exists("/contacts")) return;
    File file = _fs->open("/contacts");
    if (!file) return;
    bool full = false;
    while (!full) {
      ContactInfo c;
      uint8_t pub_key[32], unused;
      uint32_t reserved;
      bool ok = (file.read(pub_key, 32) == 32);
      ok = ok && (file.read((uint8_t*)&c.name, 32) == 32);
      ok = ok && (file.read(&c.type, 1) == 1);
      ok = ok && (file.read(&c.flags, 1) == 1);
      ok = ok && (file.read(&unused, 1) == 1);
      ok = ok && (file.read((uint8_t*)&reserved, 4) == 4);
      ok = ok && (file.read((uint8_t*)&c.out_path_len, 1) == 1);
      ok = ok && (file.read((uint8_t*)&c.last_advert_timestamp, 4) == 4);
      ok = ok && (file.read(c.out_path, 64) == 64);
      if (!ok) break;
      c.gps_lat = c.gps_lon = 0;
      c.id = mesh::Identity(pub_key);
      c.lastmod = 0;
      if (!addContact(c)) full = true;
    }
    file.close();
  }

  void saveContacts() {
    File file = _fs->open("/contacts", "w", true);
    if (!file) return;
    ContactsIterator iter = startContactsIterator();
    ContactInfo c;
    uint8_t unused = 0;
    uint32_t reserved = 0;
    while (iter.hasNext(this, c)) {
      bool ok = (file.write(c.id.pub_key, 32) == 32);
      ok = ok && (file.write((uint8_t*)&c.name, 32) == 32);
      ok = ok && (file.write(&c.type, 1) == 1);
      ok = ok && (file.write(&c.flags, 1) == 1);
      ok = ok && (file.write(&unused, 1) == 1);
      ok = ok && (file.write((uint8_t*)&reserved, 4) == 4);
      ok = ok && (file.write((uint8_t*)&c.out_path_len, 1) == 1);
      ok = ok && (file.write((uint8_t*)&c.last_advert_timestamp, 4) == 4);
      ok = ok && (file.write(c.out_path, 64) == 64);
      if (!ok) break;
    }
    file.close();
  }

protected:
  float getAirtimeBudgetFactor() const override { return 1.0f; }
  int calcRxDelay(float score, uint32_t air_time) const override { return 0; }
  bool allowPacketForward(const mesh::Packet* packet) override { return false; }   // a handheld is not a repeater

  void onDiscoveredContact(ContactInfo& contact, bool is_new, uint8_t path_len, const uint8_t* path) override {
    // No GPS fix yet and our clock is unset: borrow the time from the advert's sender.
    // Messages carry timestamps and receivers drop anything older than what they have seen.
    uint32_t ours = getRTCClock()->getCurrentTime();
    if (ours < 1700000000UL && contact.last_advert_timestamp > 1700000000UL) {
      getRTCClock()->setCurrentTime(contact.last_advert_timestamp + 1);
      chatPush("clock set from mesh");
    }
    chatPush("%s %s (%d hops)", is_new ? "New:" : "Heard:", contact.name, (int)path_len);
    lastHeardMs = millis();
    if (contact.type == ADV_TYPE_CHAT) msgs_ensure(contact.id.pub_key, contact.name, false);
    logf("advert %s type=%d new=%d hops=%d\n", contact.name, (int)contact.type, (int)is_new, (int)path_len);
    _dirty = true;
  }
  void onContactPathUpdated(const ContactInfo& contact) override {
    logf("path updated: %s len=%d\n", contact.name, (int)contact.out_path_len);
    _dirty = true;
  }
  ContactInfo* processAck(const uint8_t* data) override {
    for (auto& p : _pending) {
      if (p.ack && memcmp(data, &p.ack, 4) == 0) {
        msgs_set_status(p.conv, p.msgId, MS_DELIVERED);
        chatPush("  delivered (%lu ms)", (unsigned long)(millis() - p.sentMs));
        p.ack = 0;                       // the same ACK can arrive more than once
        break;
      }
    }
    return NULL;
  }
  void onMessageRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t ts, const char* text) override {
    lastHeardMs = millis();
    int conv = msgs_ensure(from.id.pub_key, from.name, false);
    msgs_add(conv, false, text, getRTCClock()->getCurrentTime(), MS_RECEIVED);
    chatPush("%s: %s", from.name, text);
    logf("DM from %s (%s): %s\n", from.name, pkt->isRouteDirect() ? "direct" : "flood", text);
  }
  void onCommandDataRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override {}
  void onSignedMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const uint8_t*, const char*) override {}
  void onChannelMessageRecv(const mesh::GroupChannel&, mesh::Packet* pkt, uint32_t, const char* text) override {
    lastHeardMs = millis();
    msgs_add(_pubConv, false, text, getRTCClock()->getCurrentTime(), MS_RECEIVED);
    chatPush("# %s", text);
    logf("channel msg (hops %d): %s\n", (int)pkt->path_len, text);
  }
  uint8_t onContactRequest(const ContactInfo&, uint32_t, const uint8_t*, uint8_t, uint8_t*) override { return 0; }
  void onContactResponse(const ContactInfo&, const uint8_t*, uint8_t) override {}
  uint32_t calcFloodTimeoutMillisFor(uint32_t airtime) const override { return 500 + 16.0f * airtime; }
  uint32_t calcDirectTimeoutMillisFor(uint32_t airtime, uint8_t path_len) const override {
    return 500 + (airtime * 6.0f + 250) * ((path_len & 63) + 1);
  }
  void onSendTimeout() override {
    // MeshCore reports one timeout for the most recent send: fail the oldest message still waiting.
    Pending* oldest = nullptr;
    for (auto& p : _pending) if (p.ack && (!oldest || p.sentMs < oldest->sentMs)) oldest = &p;
    if (oldest) {
      msgs_set_status(oldest->conv, oldest->msgId, MS_FAILED);
      oldest->ack = 0;
    }
    chatPush("  no ack (timed out)");
  }

public:
  char name[32] = "TDeck";
  uint32_t lastHeardMs = 0;        // last time any contact or message was heard

  DeckMesh(mesh::Radio& radio, mesh::RNG& rng, mesh::RTCClock& rtc, SimpleMeshTables& tables)
      : BaseChatMesh(radio, *new ArduinoMillis(), rng, rtc, *new StaticPoolPacketManager(16), tables) {}

  // Returns true if the radio identity was loaded or created.
  void begin(fs::FS& fs, mesh::RNG& identityRng) {
    _fs = &fs;
    BaseChatMesh::begin();
    IdentityStore store(fs, "/identity");
    char stored[32] = {0};
    if (!store.load("_main", self_id, stored, sizeof(stored))) {
      self_id = mesh::LocalIdentity(&identityRng);
      for (int n = 0; n < 10 && (self_id.pub_key[0] == 0x00 || self_id.pub_key[0] == 0xFF); n++)
        self_id = mesh::LocalIdentity(&identityRng);   // these first bytes are reserved
      store.save("_main", self_id);
      chatPush("New identity created");
    }
    if (cfg().name[0]) snprintf(name, sizeof(name), "%s", cfg().name);
    else snprintf(name, sizeof(name), "TDeck-%02X%02X", self_id.pub_key[0], self_id.pub_key[1]);
    loadContacts();
    _public = addChannel("Public", PUBLIC_GROUP_PSK);

    // Conversations: the Public channel, then every person we already know.
    _pubConv = msgs_ensure(CHANNEL_KEY, "Public", true);
    ContactsIterator iter = startContactsIterator();
    ContactInfo c;
    while (iter.hasNext(this, c)) if (c.type == ADV_TYPE_CHAT) msgs_ensure(c.id.pub_key, c.name, false);
  }

  // Writing the contact file takes ~0.5 s of flash time, so batch it instead of saving per advert.
  void tick() {
    if (_dirty && millis() - _lastSaveMs > 15000) {
      saveContacts();
      _dirty = false;
      _lastSaveMs = millis();
    }
  }

  void advertise(int delayMs) {
    auto pkt = createSelfAdvert(name, 0.0, 0.0);
    if (pkt) sendFlood(pkt, delayMs);
  }

  // Sends `text` into a conversation and tracks its delivery. The outgoing message is stored
  // first so the UI shows it immediately with "sending...".
  bool sendConv(int conv, const char* text) {
    const Conv* c = msgs_conv(conv);
    if (!c || !*text) return false;
    uint32_t now = getRTCClock()->getCurrentTime();
    uint32_t id = msgs_add(conv, true, text, now, MS_SENDING);

    if (c->channel) {
      bool ok = sendGroupMessage(now, _public->channel, name, text, strlen(text));
      msgs_set_status(conv, id, ok ? MS_SENT : MS_FAILED);   // channels have no per-message ACK
      return ok;
    }
    ContactInfo* ci = lookupContactByPubKey(c->key, 4);
    if (!ci) { msgs_set_status(conv, id, MS_FAILED); return false; }
    uint32_t ack = 0, est = 0;
    int r = sendMessage(*ci, now, 0, text, ack, est);
    if (r == MSG_SEND_FAILED) { msgs_set_status(conv, id, MS_FAILED); return false; }
    msgs_set_status(conv, id, MS_SENT);
    for (auto& p : _pending) {
      if (!p.ack) { p = { ack, conv, id, millis() }; return true; }
    }
    return true;                          // table full: delivered status just won't be tracked
  }
};

// ── Globals ──────────────────────────────────────────────────────
TFT_eSPI tft;                 // also used by ui.cpp
DeckBoard board;
// The radio shares the display's SPI object: the global SPI would re-attach the chip's default pins.
CustomSX1262 radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, TFT_eSPI::getSPIinstance());
CustomSX1262Wrapper radio_driver(radio, board);
StubSensors sensors;
static ESP32RTCClock rtc;
static StdRNG fastRng;
static SimpleMeshTables tables;
static DeckMesh* theMesh = nullptr;

static bool radioOk = false, kbOk = false;
static uint32_t loopMax = 0;

// ── GPS: only the time is used, from RMC sentences ──────────────
static bool gpsFix = false;
static uint32_t gpsFixMs = 0;
static void pollGps() {
  static char line[96];
  static int n = 0;
  int budget = 128;
  while (Serial1.available() && budget-- > 0) {
    char c = Serial1.read();
    if (c == '\n' || c == '\r') {
      if (n > 6 && line[0] == '$' && (memcmp(line + 3, "RMC", 3) == 0)) {
        line[n] = 0;
        char* f[12]; int nf = 0; char* p = line;
        while (nf < 12) { f[nf++] = p; p = strchr(p, ','); if (!p) break; *p++ = 0; }
        if (nf >= 10 && f[2][0] == 'A' && strlen(f[1]) >= 6 && strlen(f[9]) >= 6) {
          struct tm t = {};
          t.tm_hour = (f[1][0] - '0') * 10 + (f[1][1] - '0');
          t.tm_min  = (f[1][2] - '0') * 10 + (f[1][3] - '0');
          t.tm_sec  = (f[1][4] - '0') * 10 + (f[1][5] - '0');
          t.tm_mday = (f[9][0] - '0') * 10 + (f[9][1] - '0');
          t.tm_mon  = (f[9][2] - '0') * 10 + (f[9][3] - '0') - 1;
          t.tm_year = 100 + (f[9][4] - '0') * 10 + (f[9][5] - '0');
          time_t epoch = mktime(&t);                 // the ESP32 default timezone is UTC
          if (epoch > 1700000000L) {
            gpsFix = true; gpsFixMs = millis();
            int32_t diff = (int32_t)(epoch - (time_t)rtc.getCurrentTime());
            if (diff > 2 || diff < -2) rtc.setCurrentTime((uint32_t)epoch);
          }
        }
      }
      n = 0;
    } else if (n < (int)sizeof(line) - 1) {
      line[n++] = c;
    }
  }
  if (gpsFix && millis() - gpsFixMs > 30000) gpsFix = false;
}

static void pollKeyboard() {
  if (!kbOk) return;
  if (Wire.requestFrom(KB_ADDR, (uint8_t)1) != 1) return;
  char c = Wire.read();
  if (!c) return;
  ui_activity();
  ui_key(c);
}

// A stroke of the ball gives only ~2 edges, with some cross-talk on the other axis. Collect edges
// for a short window, then take one step in whichever direction had the most.
static void pollTrackball() {
  static uint32_t pu, pd, pl, pr, accU, accD, accL, accR, windowStart;
  static bool longFired = false;
  uint32_t u = isrUp, d = isrDown, l = isrLeft, r = isrRight;
  uint32_t any = (u - pu) + (d - pd) + (l - pl) + (r - pr);
  accU += u - pu; accD += d - pd; accL += l - pl; accR += r - pr;
  pu = u; pd = d; pl = l; pr = r;

  uint32_t now = millis();
  if (any && windowStart == 0) windowStart = now;
  if (windowStart && now - windowStart >= 50) {
    int dx = 0, dy = 0;
    uint32_t horiz = max(accL, accR), vert = max(accU, accD);
    if (horiz >= vert) dx = (accR >= accL) ? 1 : -1;      // ties go to sideways: the grid is wider than tall
    else dy = (accD >= accU) ? 1 : -1;
    accU = accD = accL = accR = 0;
    windowStart = 0;
    ui_activity();
    ui_nav(dx, dy);
  }

  if (clkReleased) {
    clkReleased = false;
    if (!longFired) {
      ui_activity();
      if (ui_console_active()) { if (theMesh) theMesh->advertise(0); chatPush("(advert sent)"); }
      else ui_click();
    }
    longFired = false;
  }
  if (clkDown && !longFired && now - clkDownAt > 700) {
    longFired = true;
    ui_long();
  }
}

static void buildModel(UiModel& m) {
  m.utc = rtc.getCurrentTime();
  m.clockValid = m.utc > 1700000000UL;

  uint16_t mv = board.getBattMilliVolts();
  int pct = ((int)mv - 3300) * 100 / (4150 - 3300);
  m.batteryPct = constrain(pct, 0, 100);
  m.charging = mv >= 4190;                       // heuristic: a full cell on USB reads ~4.2 V

  uint32_t age = theMesh && theMesh->lastHeardMs ? (millis() - theMesh->lastHeardMs) / 1000 : 0xFFFFFFFF;
  m.signalBars = age < 120 ? 4 : age < 600 ? 3 : age < 3600 ? 2 : age < 10800 ? 1 : 0;
  m.nodesHeard = theMesh ? theMesh->getNumContacts() : 0;
  m.unread = msgs_unread_total();
  if (theMesh) strncpy(m.nodeName, theMesh->name, sizeof(m.nodeName) - 1);

  char* o = m.consoleText;
  size_t left = sizeof(m.consoleText);
  auto add = [&](const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(o, left, fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < left) { o += n; left -= n; }
  };
  add("%s  up %lus  loop %lums  %s\n", theMesh ? theMesh->name : "-", (unsigned long)(millis() / 1000), (unsigned long)loopMax,
      gpsFix ? "GPS" : (m.clockValid ? "net time" : "no time"));
  add("Radio %s  contacts %d  batt %umV\n", radioOk ? "OK" : "FAIL", m.nodesHeard, (unsigned)mv);
  for (int i = 0; i < CHAT_LINES; i++) add("%s\n", chatBuf[i]);
  add("click=advert, hold=home");
}

static void applyRadioFromSettings() {
  const Settings& c = cfg();
  radio_driver.setParams(c.freq, c.bw, c.sf, c.cr);
  radio_driver.setTxPower(c.txPower);
}

static const UiActions uiActions = {
  /* applyRadio */ []() { applyRadioFromSettings(); },
  /* setName    */ [](const char* n) {
    if (!theMesh) return;
    snprintf(theMesh->name, sizeof(theMesh->name), "%s", n);
    theMesh->advertise(0);                 // tell the mesh about the new name right away
  },
  /* advertise  */ []() { if (theMesh) { theMesh->advertise(0); chatPush("(advert sent)"); } },
};

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(1);

  pinMode(PIN_POWERON, OUTPUT); digitalWrite(PIN_POWERON, HIGH);
  delay(150);
  pinMode(P_LORA_NSS, OUTPUT); digitalWrite(P_LORA_NSS, HIGH);
  pinMode(PIN_SD_CS, OUTPUT);  digitalWrite(PIN_SD_CS, HIGH);
  pinMode(PIN_TFT_CS, OUTPUT); digitalWrite(PIN_TFT_CS, HIGH);

  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  pinMode(PIN_BL, OUTPUT); digitalWrite(PIN_BL, HIGH);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("starting mesh...", 4, 4, 2);

  const int dirs[4] = {PIN_TB_UP, PIN_TB_DOWN, PIN_TB_LEFT, PIN_TB_RIGHT};
  void (*isrs[4])() = {onUp, onDown, onLeft, onRight};
  for (int i = 0; i < 4; i++) { pinMode(dirs[i], INPUT_PULLUP); attachInterrupt(dirs[i], isrs[i], FALLING); }
  pinMode(PIN_TB_CLICK, INPUT_PULLUP);
  attachInterrupt(PIN_TB_CLICK, onClick, CHANGE);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);
  Wire.beginTransmission(KB_ADDR);
  kbOk = (Wire.endTransmission(true) == 0);

  SPIFFS.begin(true);
  settings_begin(SPIFFS);
  msgs_init();
  ui_init();
  ui_set_actions(uiActions);
  ui_set_send_handler([](int conv, const char* text) { return theMesh && theMesh->sendConv(conv, text); });

  board.begin();
  rtc.begin();
  Serial1.setRxBufferSize(512);
  Serial1.begin(38400, SERIAL_8N1, 44, 43);   // GPS: ESP RX = GPIO44, TX = GPIO43

  SPIClass& spi = TFT_eSPI::getSPIinstance();
  radioOk = radio.std_init(&spi);
  logf("radio init: %d\n", (int)radioOk);

  if (radioOk) {
    fastRng.begin(radio_driver.getRngSeed());
    theMesh = new DeckMesh(radio_driver, fastRng, rtc, tables);
    RadioNoiseListener noise(radio);
    theMesh->begin(SPIFFS, noise);
    applyRadioFromSettings();
    theMesh->advertise(1200);
    chatPush("%s ready, advert sent", theMesh->name);
  } else {
    chatPush("radio init failed");
  }
}

void loop() {
  static uint32_t lastDraw = 0, lastLog = 0;
  uint32_t t0 = millis();

  if (theMesh) { theMesh->loop(); theMesh->tick(); }
  settings_tick();
  rtc.tick();
  pollGps();
  pollKeyboard();
  pollTrackball();

  uint32_t now = millis();
  if (now - lastDraw >= 250) {
    lastDraw = now;
    static UiModel model;
    buildModel(model);
    ui_update(model);
  }
  ui_tick();
  if (now - lastLog >= 5000) {
    lastLog = now;
    logf("up=%lus contacts=%d loopmax=%lums heap=%u ball U%lu D%lu L%lu R%lu\n", (unsigned long)(now / 1000),
         theMesh ? theMesh->getNumContacts() : 0, (unsigned long)loopMax, (unsigned)ESP.getFreeHeap(),
         (unsigned long)isrUp, (unsigned long)isrDown, (unsigned long)isrLeft, (unsigned long)isrRight);
  }
  uint32_t dt = millis() - t0;
  if (dt > loopMax) loopMax = dt;
  delay(1);
}
