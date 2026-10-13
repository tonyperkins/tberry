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
#include "storage.h"
#include "audio.h"
#include "dupcount.h"
#include "reaction.h"
#include "nodes.h"
#include "rangetest.h"

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

extern CustomSX1262 radio;               // defined below; the mesh node asks it for time on air

// ── The mesh node ────────────────────────────────────────────────
class DeckMesh : public BaseChatMesh {
  fs::FS* _fs = nullptr;
  struct Pending { uint32_t ack; int conv; uint32_t msgId; uint32_t sentMs; bool test; };
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
    nodes_seen(contact.id.pub_key, contact.name, contact.type, getRTCClock()->getCurrentTime(),
               (int)radio_driver.getLastRSSI(), radio_driver.getLastSNR(), path_len & 63,
               contact.gps_lat, contact.gps_lon);
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
        uint32_t rtt = millis() - p.sentMs;
        msgs_set_status(p.conv, p.msgId, MS_DELIVERED, (uint16_t)min<uint32_t>(65535, max<uint32_t>(1, rtt / 100)));
        chatPush("  delivered (%lu ms)", (unsigned long)rtt);
        if (p.test) finishTest(p.conv, true, rtt);
        else audio_play(SND_DELIVERED);
        p.ack = 0;                       // the same ACK can arrive more than once
        break;
      }
    }
    return NULL;
  }
  void onMessageRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t ts, const char* text) override {
    lastHeardMs = millis();
    nodes_seen(from.id.pub_key, from.name, from.type, getRTCClock()->getCurrentTime(),
               (int)radio_driver.getLastRSSI(), radio_driver.getLastSNR(), -1);
    int conv = msgs_ensure(from.id.pub_key, from.name, false);
    MsgInfo info = radioInfo(pkt, ts);
    strncpy(info.sender, from.name, sizeof(info.sender) - 1);
    msgs_add_info(conv, false, text, getRTCClock()->getCurrentTime(), MS_RECEIVED, info);
    audio_play(SND_RECEIVE);                // a direct message always chimes
    chatPush("%s: %s", from.name, text);
    logf("DM from %s (%s): %s\n", from.name, pkt->isRouteDirect() ? "direct" : "flood", text);
  }

  // What the radio knew about a received packet. SNR and RSSI belong to the last hop only.
  MsgInfo radioInfo(const mesh::Packet* pkt, uint32_t senderTs) {
    MsgInfo i = {};
    i.flags = MF_RADIO;
    i.snr4 = pkt->_snr;
    i.rssi = (int16_t)radio_driver.getLastRSSI();
    if (pkt->isRouteFlood()) {
      i.flags |= MF_FLOOD;
      i.hops = (int8_t)min<int>(pkt->getPathHashCount(), 63);
    } else {
      i.hops = -1;                          // routed straight to us: the hop list has been consumed
    }
    // How it travelled, for the message details sheet.
    i.d.senderTs = senderTs;
    i.d.payloadLen = pkt->payload_len;
    i.d.airMs = (uint16_t)min<uint32_t>(65535, radio.getTimeOnAir(pkt->getRawLength()) / 1000);
    uint8_t h[MAX_HASH_SIZE];
    pkt->calculatePacketHash(h);
    memcpy(i.d.hash4, h, 4);
    if (pkt->isRouteFlood()) {
      i.d.pathSize = pkt->getPathHashSize();
      i.d.pathCount = (uint8_t)min<int>(pkt->getPathHashCount(), 8);
      memcpy(i.d.path, pkt->path, min<size_t>(sizeof(i.d.path), (size_t)i.d.pathCount * i.d.pathSize));
    }
    return i;
  }

  // Does `text` address this node, as @[OurName]? Compared on the raw text, because the stored copy
  // has had joiners and variation selectors removed.
  bool mentionsMe(const char* text) const {
    char needle[40];
    snprintf(needle, sizeof(needle), "@[%s]", name);
    return strstr(text, needle) != nullptr;
  }
  void onCommandDataRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override {}
  void onSignedMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const uint8_t*, const char*) override {}
  void onChannelMessageRecv(const mesh::GroupChannel&, mesh::Packet* pkt, uint32_t ts, const char* text) override {
    lastHeardMs = millis();
    // Channel text arrives as "Sender: body". Keep the sender exactly as sent so a reply can mention
    // them byte for byte, and store the body on its own.
    MsgInfo info = radioInfo(pkt, ts);
    const char* body = text;
    const char* sep = strstr(text, ": ");
    if (sep && sep > text && (size_t)(sep - text) < sizeof(info.sender)) {
      memcpy(info.sender, text, sep - text);
      info.sender[sep - text] = 0;
      body = sep + 2;
    }
    char ra[40], re[24], rc[12];
    bool reaction = parseReaction(body, ra, sizeof(ra), re, sizeof(re), rc, sizeof(rc));
    {                                      // research log for TDOS-008, see storage.h
      uint8_t h8[MAX_HASH_SIZE];
      pkt->calculatePacketHash(h8);
      storage_log_idrow(getRTCClock()->getCurrentTime(), ts, (int)pkt->getPathHashCount(), h8, text);
    }
    bool mention = !reaction && mentionsMe(text);      // a reaction to us is shown, but does not chime
    if (mention) info.flags |= MF_MENTION;
    msgs_add_info(_pubConv, false, body, getRTCClock()->getCurrentTime(), MS_RECEIVED, info);
    // The channel is silent unless the owner asked for more (TDOS-006).
    uint8_t policy = cfg().channelSound;
    if (!reaction && (policy == 2 || (policy == 1 && mention))) audio_play(SND_RECEIVE);
    chatPush("# %s", text);
    logf("channel msg (hops %d)%s: %s\n", (int)pkt->getPathHashCount(), mention ? " MENTION" : "", text);
  }
  uint8_t onContactRequest(const ContactInfo&, uint32_t, const uint8_t*, uint8_t, uint8_t*) override { return 0; }
  void onContactResponse(const ContactInfo&, const uint8_t*, uint8_t) override {}
  uint32_t calcFloodTimeoutMillisFor(uint32_t airtime) const override { return 500 + 16.0f * airtime; }
  uint32_t calcDirectTimeoutMillisFor(uint32_t airtime, uint8_t path_len) const override {
    return 500 + (airtime * 6.0f + 250) * ((path_len & 63) + 1);
  }
  // The radio's "last packet" values are those of the ACK that has just been processed.
  void finishTest(int conv, bool ok, uint32_t rttMs) {
    const Conv* c = msgs_conv(conv);
    SigResult r = {};
    r.ts = getRTCClock()->getCurrentTime();
    if (c) strncpy(r.name, c->name, sizeof(r.name) - 1);
    r.ok = ok;
    r.rttMs = rttMs;
    r.hops = -1;
    if (ok) {
      r.rssi = (int16_t)radio_driver.getLastRSSI();
      r.snr4 = (int8_t)constrain((int)lroundf(radio_driver.getLastSNR() * 4.0f), -128, 127);
      ContactInfo* ci = c ? lookupContactByPubKey(c->key, 4) : nullptr;
      if (ci && ci->out_path_len != 0xFF) r.hops = ci->out_path_len & 63;
      if (c) nodes_seen(c->key, nullptr, 0, r.ts, r.rssi, r.snr4 / 4.0f, r.hops);
    }
    sig_finish(r);
    storage_log_signal(r.ts, r.name, ok, rttMs, r.hops, r.rssi, r.snr4 / 4.0f);
    if (ok) audio_play(SND_TEST_OK);
  }

  void onSendTimeout() override {
    // MeshCore reports one timeout for the most recent send: fail the oldest message still waiting.
    Pending* oldest = nullptr;
    for (auto& p : _pending) if (p.ack && (!oldest || p.sentMs < oldest->sentMs)) oldest = &p;
    if (oldest) {
      msgs_set_status(oldest->conv, oldest->msgId, MS_FAILED);
      if (oldest->test) finishTest(oldest->conv, false, millis() - oldest->sentMs);
      audio_play(SND_FAILED);
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
    while (iter.hasNext(this, c)) {
      if (c.type == ADV_TYPE_CHAT) msgs_ensure(c.id.pub_key, c.name, false);
      nodes_seen(c.id.pub_key, c.name, c.type, c.last_advert_timestamp, 0, 0, -1);
    }
    nodes_positions_load(*_fs);              // adverts' positions survive a reboot
  }

  // Writing the contact file takes ~0.5 s of flash time, so batch it instead of saving per advert.
  void tick() {
    if (_dirty && millis() - _lastSaveMs > 15000) {
      saveContacts();
      nodes_positions_save(*_fs);
      _dirty = false;
      _lastSaveMs = millis();
    }
  }

  bool sendTest(int conv) {
    const Conv* c = msgs_conv(conv);
    if (!c || c->channel) return false;
    char text[40];
    snprintf(text, sizeof(text), "Range test %d", sig_total() + 1);
    sig_begin(c->name, getRTCClock()->getCurrentTime());
    if (sendConv(conv, text, true)) return true;
    SigResult r = {};                      // could not even be queued: report it as a failed test
    r.ts = getRTCClock()->getCurrentTime();
    strncpy(r.name, c->name, sizeof(r.name) - 1);
    r.hops = -1;
    sig_finish(r);
    return false;
  }

  void advertise(int delayMs) {
    auto pkt = createSelfAdvert(name, 0.0, 0.0);
    if (pkt) sendFlood(pkt, delayMs);
  }

  // Sends `text` into a conversation and tracks its delivery. The outgoing message is stored
  // first so the UI shows it immediately with "sending...".
  bool sendConv(int conv, const char* text, bool isTest = false) {
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
    audio_play(SND_SENT);
    for (auto& p : _pending) {
      if (!p.ack) { p = { ack, conv, id, millis(), isTest }; return true; }
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
CountingTables tables;                 // also read by ui.cpp for "heard N times"
static DeckMesh* theMesh = nullptr;

static bool radioOk = false, kbOk = false;
static char i2cFound[96] = "";
            // addresses that answered at boot, for diagnosis
static uint32_t loopMax = 0;          // worst pass since boot
static uint32_t loopRecent = 0;       // worst pass in the current minute
static uint32_t loopRecentShown = 0;  // the previous minute's value, which is what the UI reports

// ── GPS: only the time is used, from RMC sentences ──────────────
static bool gpsFix = false;
static uint32_t gpsFixMs = 0;
static int32_t gpsLatE6 = 0, gpsLonE6 = 0;      // last position, micro-degrees
static uint32_t gpsByteMs = 0;                   // when the module last sent anything at all
static bool gpsPosValid = false;                 // a trustworthy position (see evalGpsValidity)
static uint32_t gpsPosMs = 0;                    // when it was last confirmed
// ── GPS power policy (TDOS-009) ──────────────────────────────────
// The module is a u-blox M10 (it identifies itself as "SPG 5.10"). "Controlled GNSS stop" idles the
// receiver while leaving the UART alive, so there is nothing to wake and no way to lock it out; it
// keeps its satellite data, so starting again is quick.
//
// At every boot the GPS runs for a start-up window whatever the setting says, because this device
// has no clock chip and GPS is the quickest source of time. The window ends when GPS time arrives
// or after GPS_WINDOW_MS. After that the setting is respected.
static constexpr uint32_t GPS_WINDOW_MS = 120000;
static bool gpsRunning = true;               // the module starts up tracking
static bool gpsWindowOpen = true;
static bool gpsTimeSeen = false;             // a valid GPS time has set the clock this session

static void ubxSend(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
  uint8_t head[6] = { 0xB5, 0x62, cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
  uint8_t a = 0, b = 0;                      // Fletcher checksum over class, id, length and payload
  for (int i = 2; i < 6; i++) { a += head[i]; b += a; }
  for (uint16_t i = 0; i < len; i++) { a += payload[i]; b += a; }
  Serial1.write(head, sizeof(head));
  if (len) Serial1.write(payload, len);
  Serial1.write(a);
  Serial1.write(b);
}

static void gpsSetRunning(bool run);

static void gpsPolicyTick() {
  if (gpsWindowOpen && (gpsTimeSeen || millis() > GPS_WINDOW_MS)) gpsWindowOpen = false;
  gpsSetRunning(cfg().gpsEnabled || gpsWindowOpen);
}

// What the GPS module has told us beyond the position, for the Status screen.
struct GpsInfo {
  uint8_t fixType = 0;            // from GSA: 1 none, 2 = 2D, 3 = 3D
  uint8_t quality = 0;            // from GGA: 0 none, 1 GPS, 2 differential
  uint8_t satsUsed = 0;
  float hdop = 0, altM = 0, speedKmh = 0, courseDeg = 0;
  struct { char talker[3]; uint8_t inView; uint32_t ms; } view[6] = {};   // one slot per constellation
} gpsInfo;
static float speedHist[3] = {0, 0, 0};           // last three speed readings, for smoothing

static uint8_t satsInView() {
  uint32_t now = millis();
  int total = 0;
  for (auto& v : gpsInfo.view) if (v.talker[0] && now - v.ms < 15000) total += v.inView;
  return (uint8_t)min(total, 99);
}

static double nmeaDegrees(const char* s, char hemi) {
  double v = atof(s);
  double d = floor(v / 100.0) + fmod(v, 100.0) / 60.0;
  return (hemi == 'S' || hemi == 'W') ? -d : d;
}

// A position is only trusted when the module says it has a fix (GGA quality), is using enough
// satellites, and reports a 2D or 3D solution (GSA). RMC "A" on its own is not enough: modules keep
// sending it for a while after the sky view is lost.
static void evalGpsValidity() {
  bool valid = gpsFix && gpsInfo.quality > 0 && gpsInfo.satsUsed >= 3 && gpsInfo.fixType >= 2;
  if (valid && !gpsPosValid) audio_play(SND_GPS_LOCK);
  gpsPosValid = valid;
  if (valid) {
    gpsPosMs = millis();
  } else {                                       // do not leave stale numbers on the screen
    gpsInfo.speedKmh = 0;
    gpsInfo.courseDeg = 0;
    gpsInfo.altM = 0;
    for (auto& v : speedHist) v = 0;
  }
}

static void gpsSetRunning(bool run) {
  if (run == gpsRunning) return;
  gpsRunning = run;
  static const uint8_t stopMsg[4]  = { 0x00, 0x00, 0x08, 0x00 };   // UBX-CFG-RST: hot start, controlled GNSS stop
  static const uint8_t startMsg[4] = { 0x00, 0x00, 0x09, 0x00 };   //              ... controlled GNSS start
  ubxSend(0x06, 0x04, run ? startMsg : stopMsg, 4);
  if (!run) {                                  // nothing the module says now is a position
    gpsFix = false;
    gpsPosValid = false;
    gpsInfo.fixType = 0; gpsInfo.quality = 0; gpsInfo.satsUsed = 0; gpsInfo.hdop = 0;
    gpsInfo.speedKmh = 0; gpsInfo.courseDeg = 0; gpsInfo.altM = 0;
    for (auto& v : speedHist) v = 0;
    for (auto& v : gpsInfo.view) v.talker[0] = 0;
  }
}

// One complete sentence, e.g. "$GNRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,,,A*6A".
static void handleNmea(char* line, int n) {
  if (n < 10 || line[0] != '$') return;
  if (!gpsRunning) return;                       // stopped: whatever it still prints is not data
  line[n] = 0;
  char* star = strchr(line, '*');
  if (star) *star = 0;
  char* f[24];
  int nf = 0;
  char* p = line;
  while (nf < 24) { f[nf++] = p; p = strchr(p, ','); if (!p) break; *p++ = 0; }
  const char* type = line + 3;                   // after "$GN" / "$GP" / "$GL" ...
  auto num = [&](int i) { return i < nf ? atof(f[i]) : 0.0; };

  if (memcmp(type, "RMC", 3) == 0) {
    if (nf < 10 || f[2][0] != 'A' || strlen(f[1]) < 6 || strlen(f[9]) < 6) return;
    struct tm t = {};
    t.tm_hour = (f[1][0] - '0') * 10 + (f[1][1] - '0');
    t.tm_min  = (f[1][2] - '0') * 10 + (f[1][3] - '0');
    t.tm_sec  = (f[1][4] - '0') * 10 + (f[1][5] - '0');
    t.tm_mday = (f[9][0] - '0') * 10 + (f[9][1] - '0');
    t.tm_mon  = (f[9][2] - '0') * 10 + (f[9][3] - '0') - 1;
    t.tm_year = 100 + (f[9][4] - '0') * 10 + (f[9][5] - '0');
    time_t epoch = mktime(&t);                   // the ESP32 default timezone is UTC
    if (nf >= 7 && strlen(f[3]) > 4 && strlen(f[5]) > 5) {
      gpsLatE6 = (int32_t)lround(nmeaDegrees(f[3], f[4][0]) * 1e6);
      gpsLonE6 = (int32_t)lround(nmeaDegrees(f[5], f[6][0]) * 1e6);
    }
    // Speed over ground comes from Doppler and wanders by several km/h when few satellites are in
    // use, so average the last three readings.
    speedHist[2] = speedHist[1]; speedHist[1] = speedHist[0];
    speedHist[0] = (float)(num(7) * 1.852);       // knots -> km/h
    gpsInfo.speedKmh = (speedHist[0] + speedHist[1] + speedHist[2]) / 3.0f;
    gpsInfo.courseDeg = (float)num(8);
    if (epoch > 1700000000L) {
      gpsTimeSeen = true;
      gpsFix = true;
      gpsFixMs = millis();
      int32_t diff = (int32_t)(epoch - (time_t)rtc.getCurrentTime());
      if (diff > 2 || diff < -2) rtc.setCurrentTime((uint32_t)epoch);
    }
    evalGpsValidity();
  } else if (memcmp(type, "GGA", 3) == 0) {
    gpsInfo.quality = (uint8_t)num(6);
    gpsInfo.satsUsed = (uint8_t)num(7);
    gpsInfo.hdop = (float)num(8);
    gpsInfo.altM = (float)num(9);
    evalGpsValidity();
  } else if (memcmp(type, "GSA", 3) == 0) {
    gpsInfo.fixType = (uint8_t)num(2);
    evalGpsValidity();
  } else if (memcmp(type, "GSV", 3) == 0) {
    // field 3 is the number of satellites in view for this constellation (the talker id)
    const char* talker = line + 1;
    int slot = -1;
    for (int k = 0; k < 6; k++) {
      if (gpsInfo.view[k].talker[0] && memcmp(gpsInfo.view[k].talker, talker, 2) == 0) { slot = k; break; }
      if (slot < 0 && !gpsInfo.view[k].talker[0]) slot = k;
    }
    if (slot >= 0) {
      memcpy(gpsInfo.view[slot].talker, talker, 2);
      gpsInfo.view[slot].talker[2] = 0;
      gpsInfo.view[slot].inView = (uint8_t)num(3);
      gpsInfo.view[slot].ms = millis();
    }
  }
}

static void pollGps() {
  static char line[120];
  static int n = 0;
  int budget = 160;
  while (Serial1.available() && budget-- > 0) {
    char c = Serial1.read();
    gpsByteMs = millis();

    if (c == '\n' || c == '\r') {
      if (n > 0) handleNmea(line, n);
      n = 0;
    } else if (n < (int)sizeof(line) - 1) {
      line[n++] = c;
    }
  }
  if (gpsFix && millis() - gpsFixMs > 10000) {   // no valid RMC for ten seconds: lost
    gpsFix = false;
    gpsInfo.fixType = 0;
    evalGpsValidity();
  }
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
  m.batteryMv = mv;
  {
    static bool warned = false;      // one warning per discharge, re-armed once recharged
    if (m.batteryPct <= 15 && !m.charging && !warned) { warned = true; audio_play(SND_LOW_BATTERY); }
    if (m.batteryPct >= 25 || m.charging) warned = false;
  }
  m.uptimeSec = millis() / 1000;
  m.packetsRx = radio_driver.getPacketsRecv();
  m.packetsTx = radio_driver.getPacketsSent();
  m.rxErrors = radio_driver.getPacketsRecvErrors();
  m.airtimeMs = theMesh ? theMesh->getTotalAirTime() : 0;
  m.loopWorstMs = max(loopRecentShown, loopRecent);
  m.heapKb = ESP.getFreeHeap() / 1024;
  m.psramKb = ESP.getFreePsram() / 1024;
  m.timeSource = gpsFix ? 2 : (m.clockValid ? 1 : 0);
  m.sdReady = storage_ready();
  m.hasPos = gpsPosValid && (gpsLatE6 || gpsLonE6);
  bool gpsOff = !cfg().gpsEnabled && !gpsWindowOpen;
  m.gpsState = gpsOff ? 3 : gpsPosValid ? 2 : (gpsByteMs && millis() - gpsByteMs < 5000 ? 1 : 0);
  m.gpsWindowSec = (!cfg().gpsEnabled && gpsWindowOpen && millis() < GPS_WINDOW_MS) ? (GPS_WINDOW_MS - millis()) / 1000 : 0;
  m.gpsFixType = gpsPosValid ? gpsInfo.fixType : 0;
  m.gpsSatsUsed = gpsInfo.satsUsed;
  m.gpsSatsView = satsInView();
  m.gpsHdop = gpsInfo.hdop;
  m.gpsAltM = gpsInfo.altM;
  m.gpsSpeedKmh = gpsInfo.speedKmh;
  m.gpsCourse = gpsInfo.courseDeg;
  m.gpsFixAgeSec = gpsPosMs ? (millis() - gpsPosMs) / 1000 : 0;
  m.latE6 = gpsLatE6;
  m.lonE6 = gpsLonE6;
  m.sdFreeMb = storage_free_mb();
  if (storage_ready()) snprintf(m.storage, sizeof(m.storage), "SD, %lu MB free", (unsigned long)storage_free_mb());
  else snprintf(m.storage, sizeof(m.storage), "No SD card");
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
  /* sendTest   */ [](int conv) { return theMesh && theMesh->sendTest(conv); },
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
  {                                          // which chips are on the I2C bus (touch, keyboard, and any RTC)
    size_t n = 0;
    for (uint8_t a = 0x08; a < 0x78 && n + 6 < sizeof(i2cFound); a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission(true) == 0) n += snprintf(i2cFound + n, sizeof(i2cFound) - n, "0x%02X ", a);
    }
  }

  SPIFFS.begin(true);
  settings_begin(SPIFFS);
  msgs_init();
  storage_begin(TFT_eSPI::getSPIinstance(), PIN_SD_CS);   // message history; fine if no card
  audio_begin();
  audio_set_volume(cfg().volume);
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
    storage_load_messages();            // conversations now exist: restore their history
    theMesh->advertise(1200);
    chatPush("%s ready, advert sent", theMesh->name);
    audio_play(SND_BOOT);
  } else {
    chatPush("radio init failed");
  }
}

void loop() {
  static uint32_t lastDraw = 0, lastLog = 0;
  uint32_t t0 = millis();

  if (theMesh) { theMesh->loop(); theMesh->tick(); }
  settings_tick();
  storage_tick();
  rtc.tick();
  gpsPolicyTick();
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
    logf("i2c: %s  lvgl free %lu\n", i2cFound, (unsigned long)ui_lvgl_free());
    logf("up=%lus contacts=%d loopmax=%lums heap=%u ball U%lu D%lu L%lu R%lu\n", (unsigned long)(now / 1000),
         theMesh ? theMesh->getNumContacts() : 0, (unsigned long)loopMax, (unsigned)ESP.getFreeHeap(),
         (unsigned long)isrUp, (unsigned long)isrDown, (unsigned long)isrLeft, (unsigned long)isrRight);
  }
  uint32_t dt = millis() - t0;
  if (dt > loopMax) loopMax = dt;
  if (dt > loopRecent) loopRecent = dt;
  static uint32_t minuteStart = 0;
  if (millis() - minuteStart > 60000) {
    minuteStart = millis();
    loopRecentShown = loopRecent;
    loopRecent = 0;
  }
  delay(1);
}
