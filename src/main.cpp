// tdeck-os, step 1: MeshCore chat node on the T-Deck Plus hardware layer.
//
// What this proves: the radio speaks MeshCore (adverts, contacts, encrypted DMs, Public channel),
// keeps its identity and contacts across reboots, and nothing in the main loop can block.
// The screen is a plain text console for now; the real UI comes in step 2.
//
// Controls: type with the keyboard, Enter sends to the selected recipient,
// trackball up/down selects the recipient (Public channel, then known contacts).
//
// Rules carried over from earlier firmware trouble:
//   * the trackball is interrupt-driven, never polled
//   * serial logging goes through logf(), which can never block on an unread USB port
//   * radio work is non-blocking (MeshCore's loop handles it)

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

// ── Trackball (interrupts count edges; the loop consumes the counters) ──
static volatile uint32_t isrUp, isrDown, isrLeft, isrRight, isrClick;
static void IRAM_ATTR onUp()    { isrUp++; }
static void IRAM_ATTR onDown()  { isrDown++; }
static void IRAM_ATTR onLeft()  { isrLeft++; }
static void IRAM_ATTR onRight() { isrRight++; }
static void IRAM_ATTR onClick() {
  static uint32_t last;
  uint32_t now = micros();
  if (digitalRead(PIN_TB_CLICK) == LOW && now - last > 20000) { isrClick++; last = now; }
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
  uint32_t _expectedAck = 0;
  uint32_t _sentAtMs = 0;
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
    logf("advert %s type=%d new=%d hops=%d\n", contact.name, (int)contact.type, (int)is_new, (int)path_len);
    _dirty = true;
  }
  void onContactPathUpdated(const ContactInfo& contact) override {
    logf("path updated: %s len=%d\n", contact.name, (int)contact.out_path_len);
    _dirty = true;
  }
  ContactInfo* processAck(const uint8_t* data) override {
    if (_expectedAck && memcmp(data, &_expectedAck, 4) == 0) {
      chatPush("  delivered (%lu ms)", (unsigned long)(millis() - _sentAtMs));
      _expectedAck = 0;
    }
    return NULL;
  }
  void onMessageRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t ts, const char* text) override {
    chatPush("%s: %s", from.name, text);
    logf("DM from %s (%s): %s\n", from.name, pkt->isRouteDirect() ? "direct" : "flood", text);
  }
  void onCommandDataRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override {}
  void onSignedMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const uint8_t*, const char*) override {}
  void onChannelMessageRecv(const mesh::GroupChannel&, mesh::Packet* pkt, uint32_t, const char* text) override {
    chatPush("# %s", text);
    logf("channel msg (hops %d): %s\n", (int)pkt->path_len, text);
  }
  uint8_t onContactRequest(const ContactInfo&, uint32_t, const uint8_t*, uint8_t, uint8_t*) override { return 0; }
  void onContactResponse(const ContactInfo&, const uint8_t*, uint8_t) override {}
  uint32_t calcFloodTimeoutMillisFor(uint32_t airtime) const override { return 500 + 16.0f * airtime; }
  uint32_t calcDirectTimeoutMillisFor(uint32_t airtime, uint8_t path_len) const override {
    return 500 + (airtime * 6.0f + 250) * ((path_len & 63) + 1);
  }
  void onSendTimeout() override { chatPush("  no ack (timed out)"); }

public:
  char name[32] = "TDeck";

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
    snprintf(name, sizeof(name), "TDeck-%02X%02X", self_id.pub_key[0], self_id.pub_key[1]);
    loadContacts();
    _public = addChannel("Public", PUBLIC_GROUP_PSK);
  }

  // Writing the contact file takes ~0.5 s of flash time, so batch it instead of saving per advert.
  void tick() {
    if (_dirty && millis() - _lastSaveMs > 15000) {
      saveContacts();
      _dirty = false;
      _lastSaveMs = millis();
    }
  }

  int chatContacts() { return recipientCount() - 1; }

  void advertise(int delayMs) {
    auto pkt = createSelfAdvert(name, 0.0, 0.0);
    if (pkt) sendFlood(pkt, delayMs);
  }

  // The n-th (from 0) contact that is a person. Uses the iterator because the raw array starts
  // with reserved anonymous slots that getContactByIdx() would expose.
  bool chatAt(int n, ContactInfo& out) {
    ContactsIterator iter = startContactsIterator();
    ContactInfo c;
    int k = 0;
    while (iter.hasNext(this, c)) {
      if (c.type != ADV_TYPE_CHAT) continue;
      if (k == n) { out = c; return true; }
      k++;
    }
    return false;
  }

  // recipient index 0 = Public channel, 1.. = people
  int recipientCount() {
    ContactInfo c;
    int n = 1;
    while (chatAt(n - 1, c)) n++;
    return n;
  }
  bool recipientName(int idx, char* out, size_t len) {
    if (idx == 0) { snprintf(out, len, "Public channel"); return true; }
    ContactInfo c;
    if (!chatAt(idx - 1, c)) return false;
    snprintf(out, len, "%s", c.name);
    return true;
  }

  bool sendTo(int idx, const char* text) {
    uint32_t now = getRTCClock()->getCurrentTime();
    if (idx == 0) {
      bool ok = sendGroupMessage(now, _public->channel, name, text, strlen(text));
      chatPush("%s# %s", ok ? "" : "FAILED ", text);
      return ok;
    }
    ContactInfo c;
    if (!chatAt(idx - 1, c)) return false;
    ContactInfo* ci = lookupContactByPubKey(c.id.pub_key, PUB_KEY_SIZE);
    if (!ci) return false;
    uint32_t est;
    int r = sendMessage(*ci, now, 0, text, _expectedAck, est);
    if (r == MSG_SEND_FAILED) { chatPush("send failed"); return false; }
    _sentAtMs = millis();
    chatPush("> %s: %s", c.name, text);
    return true;
  }
};

// ── Globals ──────────────────────────────────────────────────────
static TFT_eSPI tft;
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
static int recipient = 0;
static char typed[96];
static int typedLen = 0;
static uint32_t seenUp, seenDown, seenClick;
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
  if (c == '\r' || c == '\n') {
    if (typedLen > 0 && theMesh) {
      typed[typedLen] = 0;
      theMesh->sendTo(recipient, typed);
      typedLen = 0;
    }
  } else if (c == 8 || c == 127) {
    if (typedLen > 0) typedLen--;
  } else if (c >= 32 && c < 127 && typedLen < (int)sizeof(typed) - 1) {
    typed[typedLen++] = c;
  }
}

static void pollTrackball() {
  uint32_t u = isrUp, d = isrDown, k = isrClick;
  int n = theMesh ? theMesh->recipientCount() : 1;
  if (d != seenDown) { seenDown = d; recipient = (recipient + 1) % n; }
  if (u != seenUp)   { seenUp = u;   recipient = (recipient + n - 1) % n; }
  if (k != seenClick) { seenClick = k; if (theMesh) theMesh->advertise(0); chatPush("(advert sent)"); }
}

static void drawScreen() {
  static int row;
  row = 0;
  auto line = [&](uint16_t color, const char* fmt, ...) {
    char b[80];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    tft.setTextColor(color, TFT_BLACK);
    tft.setTextPadding(320);
    tft.drawString(b, 4, 2 + row * 17, 2);
    row++;
  };
  char who[40] = "?";
  if (theMesh) theMesh->recipientName(recipient, who, sizeof(who));
  line(TFT_CYAN, "%s  up %lus  loop %lums", theMesh ? theMesh->name : "-", (unsigned long)(millis() / 1000), (unsigned long)loopMax);
  uint32_t tnow = rtc.getCurrentTime();
  line(radioOk ? TFT_GREEN : TFT_RED, "Radio %s  contacts %d  %02u:%02u:%02u UTC %s", radioOk ? "OK" : "FAIL",
       theMesh ? theMesh->getNumContacts() : 0, (unsigned)((tnow / 3600) % 24), (unsigned)((tnow / 60) % 60),
       (unsigned)(tnow % 60), gpsFix ? "GPS" : (tnow > 1700000000UL ? "net" : "no time"));
  line(TFT_YELLOW, "To: %s  (%d/%d)", who, recipient + 1, theMesh ? theMesh->recipientCount() : 1);
  line(TFT_WHITE, "> %s_", typed);
  line(TFT_DARKGREY, "ball U%lu D%lu  click=%lu (sends advert)", (unsigned long)isrUp, (unsigned long)isrDown, (unsigned long)isrClick);
  for (int i = 0; i < CHAT_LINES; i++) line(TFT_WHITE, "%s", chatBuf[i]);
}

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
  attachInterrupt(PIN_TB_CLICK, onClick, FALLING);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);
  Wire.beginTransmission(KB_ADDR);
  kbOk = (Wire.endTransmission(true) == 0);

  board.begin();
  rtc.begin();
  Serial1.setRxBufferSize(512);
  Serial1.begin(38400, SERIAL_8N1, 44, 43);   // GPS: ESP RX = GPIO44, TX = GPIO43

  SPIClass& spi = TFT_eSPI::getSPIinstance();
  radioOk = radio.std_init(&spi);
  logf("radio init: %d\n", (int)radioOk);

  if (radioOk) {
    fastRng.begin(radio_driver.getRngSeed());
    SPIFFS.begin(true);
    theMesh = new DeckMesh(radio_driver, fastRng, rtc, tables);
    RadioNoiseListener noise(radio);
    theMesh->begin(SPIFFS, noise);
    radio_driver.setParams(910.525f, 62.5f, 7, 5);
    radio_driver.setTxPower(22);
    theMesh->advertise(1200);
    chatPush("%s ready, advert sent", theMesh->name);
  } else {
    chatPush("radio init failed");
  }
  tft.fillScreen(TFT_BLACK);
}

void loop() {
  static uint32_t lastDraw = 0, lastLog = 0;
  uint32_t t0 = millis();

  if (theMesh) { theMesh->loop(); theMesh->tick(); }
  rtc.tick();
  pollGps();
  pollKeyboard();
  pollTrackball();

  uint32_t now = millis();
  if (now - lastDraw >= 250) { lastDraw = now; drawScreen(); }
  if (now - lastLog >= 5000) {
    lastLog = now;
    logf("up=%lus contacts=%d loopmax=%lums heap=%u\n", (unsigned long)(now / 1000),
         theMesh ? theMesh->getNumContacts() : 0, (unsigned long)loopMax, (unsigned)ESP.getFreeHeap());
  }
  uint32_t dt = millis() - t0;
  if (dt > loopMax) loopMax = dt;
  delay(1);
}
