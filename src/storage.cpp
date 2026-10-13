// Message log on the SD card: one append-only file per conversation,
//   /tdos/msgs/<first 4 key bytes in hex>.log
// holding two kinds of record: a message, and a status change for an earlier message. Appending
// is cheap and survives a power cut (a torn last record is simply ignored). Files are compacted
// when loaded if they have grown well past what the app keeps in memory.
#include "storage.h"
#include "messages.h"
#include <Arduino.h>
#include <SD.h>

static bool ready = false;
static constexpr const char* DIR_MSGS = "/tdos/msgs";

struct __attribute__((packed)) RecM { uint8_t type; uint8_t out; uint8_t status; uint8_t len; uint32_t id; uint32_t ts; };
// 'N' = message with radio info and the raw sender (TDOS-001/003). Followed by the sender, then the text.
struct __attribute__((packed)) RecN { uint8_t type; uint8_t out; uint8_t status; uint8_t len; uint32_t id; uint32_t ts;
                                      int8_t hops; int8_t snr4; int16_t rssi; uint8_t flags; uint8_t senderLen; };
struct __attribute__((packed)) RecS { uint8_t type; uint8_t status; uint16_t rtt100; uint32_t id; };   // rtt100 was padding: old files read as 0

// ── write queue (filled by the store's sink, drained by storage_tick) ──
struct Ev {
  uint8_t key[4];
  uint8_t kind;               // 'M' or 'S'
  uint8_t out, status;
  uint16_t rtt100;
  uint32_t id, ts;
  int8_t hops, snr4;
  int16_t rssi;
  uint8_t flags;
  char sender[32];
  char text[164];
};
static constexpr int QN = 48;
static Ev q[QN];
static int qHead = 0, qCount = 0;
static uint32_t lastFlushMs = 0;

static void push(const Ev& e) {
  if (qCount == QN) { qHead = (qHead + 1) % QN; qCount--; }       // full: drop the oldest
  q[(qHead + qCount) % QN] = e;
  qCount++;
}

static void sinkAdd(const uint8_t key[4], const Msg& m) {
  if (!ready) return;
  Ev e = {};
  memcpy(e.key, key, 4);
  e.kind = 'N'; e.out = m.out; e.status = m.status; e.id = m.id; e.ts = m.ts;
  e.hops = m.hops; e.snr4 = m.snr4; e.rssi = m.rssi; e.flags = m.flags;
  strncpy(e.sender, m.sender, sizeof(e.sender) - 1);
  strncpy(e.text, m.text, sizeof(e.text) - 1);
  push(e);
}
static void sinkStatus(const uint8_t key[4], uint32_t id, uint8_t status, uint16_t rtt100) {
  if (!ready) return;
  Ev e = {};
  memcpy(e.key, key, 4);
  e.kind = 'S'; e.status = status; e.id = id; e.rtt100 = rtt100;
  push(e);
}
static const MsgSink sink = { sinkAdd, sinkStatus };

static void pathFor(const uint8_t key[4], char* out, size_t n) {
  snprintf(out, n, "%s/%02x%02x%02x%02x.log", DIR_MSGS, key[0], key[1], key[2], key[3]);
}

static void writeEvent(const Ev& e) {
  char path[48];
  pathFor(e.key, path, sizeof(path));
  File f = SD.open(path, FILE_APPEND);
  if (!f) return;
  if (e.kind == 'N') {
    RecN r = { 'N', e.out, e.status, (uint8_t)strlen(e.text), e.id, e.ts, e.hops, e.snr4, e.rssi, e.flags, (uint8_t)strlen(e.sender) };
    f.write((const uint8_t*)&r, sizeof(r));
    f.write((const uint8_t*)e.sender, r.senderLen);
    f.write((const uint8_t*)e.text, r.len);
  } else {
    RecS r = { 'S', e.status, e.rtt100, e.id };
    f.write((const uint8_t*)&r, sizeof(r));
  }
  f.close();
}

// ── public ──
bool storage_begin(SPIClass& spi, int csPin) {
  msgs_set_sink(&sink);
  ready = SD.begin(csPin, spi, 10000000);
  if (ready) {
    SD.mkdir("/tdos");
    SD.mkdir(DIR_MSGS);
  }
  return ready;
}

bool storage_ready() { return ready; }

uint32_t storage_free_mb() {
  if (!ready) return 0;
  return (uint32_t)((SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024ULL));
}

// Rewrite one conversation's file from what is now in memory.
static void compact(int conv) {
  const Conv* c = msgs_conv(conv);
  if (!c) return;
  char path[48];
  pathFor(c->key, path, sizeof(path));
  SD.remove(path);
  File f = SD.open(path, FILE_WRITE);
  if (!f) return;
  for (int i = 0; i < c->count; i++) {
    const Msg* m = msgs_at(conv, i);
    RecN r = { 'N', (uint8_t)m->out, m->status, (uint8_t)strlen(m->text), m->id, m->ts, m->hops, m->snr4, m->rssi, m->flags,
               (uint8_t)strlen(m->sender) };
    f.write((const uint8_t*)&r, sizeof(r));
    f.write((const uint8_t*)m->sender, r.senderLen);
    f.write((const uint8_t*)m->text, r.len);
    if (m->rtt100) {
      RecS s = { 'S', m->status, m->rtt100, m->id };
      f.write((const uint8_t*)&s, sizeof(s));
    }
  }
  f.close();
}

void storage_load_messages() {
  if (!ready) return;
  File dir = SD.open(DIR_MSGS);
  if (!dir) return;
  while (true) {
    File f = dir.openNextFile();
    if (!f) break;
    if (f.isDirectory()) { f.close(); continue; }
    const char* name = f.name();
    const char* base = strrchr(name, '/');
    base = base ? base + 1 : name;
    unsigned k[4];
    if (strlen(base) != 12 || sscanf(base, "%2x%2x%2x%2x", &k[0], &k[1], &k[2], &k[3]) != 4) { f.close(); continue; }
    uint8_t key[4] = { (uint8_t)k[0], (uint8_t)k[1], (uint8_t)k[2], (uint8_t)k[3] };

    bool channel = memcmp(key, CHANNEL_KEY, 4) == 0;
    int conv = msgs_find(key);
    if (conv < 0) conv = msgs_ensure(key, channel ? "Public" : "(unknown)", channel);
    if (conv < 0) { f.close(); continue; }

    int records = 0;
    while (f.available()) {
      uint8_t type = f.peek();
      if (type == 'M') {
        RecM r;
        if (f.read((uint8_t*)&r, sizeof(r)) != sizeof(r)) break;
        char text[164] = {0};
        if (r.len >= sizeof(text) || f.read((uint8_t*)text, r.len) != r.len) break;
        msgs_add_loaded(conv, r.id, r.out, text, r.ts, r.status);
        records++;
      } else if (type == 'N') {
        RecN r;
        if (f.read((uint8_t*)&r, sizeof(r)) != sizeof(r)) break;
        MsgInfo info = {};
        char text[164] = {0};
        if (r.senderLen >= sizeof(info.sender) || r.len >= sizeof(text)) break;
        if (r.senderLen && f.read((uint8_t*)info.sender, r.senderLen) != r.senderLen) break;
        if (f.read((uint8_t*)text, r.len) != r.len) break;
        info.hops = r.hops; info.snr4 = r.snr4; info.rssi = r.rssi; info.flags = r.flags;
        msgs_add_loaded_info(conv, r.id, r.out, text, r.ts, r.status, info);
        records++;
      } else if (type == 'S') {
        RecS r;
        if (f.read((uint8_t*)&r, sizeof(r)) != sizeof(r)) break;
        msgs_set_status_loaded(conv, r.id, r.status, r.rtt100);
        records++;
      } else {
        break;                                         // damaged tail: ignore the rest
      }
    }
    f.close();
    if (records > 2 * MSGS_PER_CONV) compact(conv);
  }
  dir.close();
  // Anything loaded is history, not news.
  for (int i = 0; i < MAX_CONVS; i++) msgs_mark_read(i);
}

// One line per range test, for later analysis: time, who, result, round trip, hops, RSSI, SNR.
void storage_log_signal(uint32_t ts, const char* name, bool ok, uint32_t rttMs, int hops, int rssi, float snr) {
  if (!ready) return;
  bool fresh = !SD.exists("/tdos/signal.csv");
  File f = SD.open("/tdos/signal.csv", FILE_APPEND);
  if (!f) return;
  if (fresh) f.println("utc,node,result,rtt_ms,hops,rssi_dbm,snr_db");
  f.printf("%lu,%s,%s,%lu,%d,%d,%.1f\n", (unsigned long)ts, name, ok ? "ok" : "timeout", (unsigned long)rttMs, hops, rssi, snr);
  f.close();
}

void storage_tick() {
  if (!ready || qCount == 0) return;
  uint32_t now = millis();
  if (now - lastFlushMs < 400) return;
  lastFlushMs = now;
  // At most two records per pass: each is an open, a write and a close on the card.
  for (int n = 0; n < 2 && qCount > 0; n++) {
    writeEvent(q[qHead]);
    qHead = (qHead + 1) % QN;
    qCount--;
  }
}
