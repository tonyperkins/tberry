#include "covsync.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include "settings.h"
#include "storage.h"
#include "updater.h"
#include "version.h"
#include "ca_bundle.h"

static constexpr int BATCH_ROWS = 50;
static constexpr int MAX_BATCHES = 40;           // per WiFi session: 2000 rows, then a pause
static constexpr uint32_t RETRY_OK_MS = 10UL * 60000;
static constexpr uint32_t RETRY_NOWIFI_MS = 5UL * 60000;
static constexpr uint32_t RETRY_ERR_MS = 15UL * 60000;
static constexpr uint32_t MIN_BLOCK = 45000;      // largest free block needed before WiFi + TLS start (the TLS buffers alone are ~34 KB)
static constexpr uint32_t PARKED_FOR_MS = 90000;

enum Phase { PH_IDLE, PH_CONNECTING, PH_READY, PH_POSTING, PH_POSTED, PH_FINISHING, PH_END };
enum Cmd { CMD_NONE, CMD_POST, CMD_FINISH };
enum Result { RES_NONE, RES_OK, RES_NOWIFI, RES_CONNECT, RES_TIME, RES_HTTP, RES_NET, RES_CONFIG, RES_MEMORY };

static volatile Phase phase = PH_IDLE;
static volatile Cmd cmd = CMD_NONE;
static volatile Result result = RES_NONE;
static volatile int httpCode = 0;
static char body[9000];                       // the JSON batch being posted
static size_t bodyLen = 0;
static uint32_t batchBytes = 0;               // log bytes the batch covers, once accepted
static int batchRows = 0;
static int batchesDone = 0;
static uint32_t rowsSent = 0;                 // this session
static uint32_t sentOffset = 0;               // loaded from the card when a session starts
static uint32_t nextAttemptMs = 60000;        // not in the first minute
static uint32_t parkedSinceMs = 0;
static uint32_t lastEndMs = 0;
static Result lastResult = RES_NONE;
static int lastHttp = 0;
static uint32_t lastRows = 0;
static bool wantNow = false;
static bool failed = false;                   // the receiver (or the network) refused a batch
static bool taskAlive = false;
static char device[40] = "tdeck";
static char status[72] = "Not tried yet";

void covsync_set_device(const char* name) { snprintf(device, sizeof(device), "%s", name); }
bool covsync_busy() { return phase != PH_IDLE; }

// ── provisioning: a file on the card with the address on line 1 and the key on line 2 ──
void covsync_begin() {
  char url[96], key[80];
  if (storage_read_line("/tdos/upload.conf", url, sizeof(url), 0) && storage_read_line("/tdos/upload.conf", key, sizeof(key), 1)
      && url[0] && key[0] && strlen(key) < sizeof(cfg().covKey)) {   // a key that does not fit would be cut short and rejected
    snprintf(cfg().covUrl, sizeof(cfg().covUrl), "%s", url);
    snprintf(cfg().covKey, sizeof(cfg().covKey), "%s", key);
    settings_changed();
    storage_remove("/tdos/upload.conf");                 // the key should not sit on a card that can be lost
  }
}

uint32_t covsync_pending_bytes() {
  uint32_t size = storage_coverage_size(), sent = storage_coverage_sent();
  return size > sent ? size - sent : 0;
}

// ── building a batch (main loop: reads the card) ──
static bool numeric(const char* s) {
  if (!*s) return false;
  for (; *s; s++) if (!((*s >= '0' && *s <= '9') || *s == '-' || *s == '.')) return false;
  return true;
}
static void safeText(char* s) { for (; *s; s++) if (!(isalnum((unsigned char)*s) || *s == ';' || *s == '=' || *s == '.' || *s == '-' || *s == '_')) *s = '_'; }

static bool buildBatch() {
  static char raw[7000];
  uint32_t off = sentOffset;
  size_t got = storage_coverage_read(off, raw, sizeof(raw) - 1);
  if (!got) return false;
  raw[got] = 0;
  bodyLen = snprintf(body, sizeof(body), "{\"device\":\"%s\",\"fw\":\"%s\",\"data\":[", device, TDOS_VERSION);
  char* p = raw;
  int rows = 0;
  uint32_t used = 0;
  bool firstOut = true;
  while (rows < BATCH_ROWS) {
    char* nl = strchr(p, '\n');
    if (!nl) break;                                      // a partial last line stays for next time
    *nl = 0;
    uint32_t lineBytes = (nl - p) + 1;
    char line[160];
    snprintf(line, sizeof(line), "%s", p);
    used += lineBytes;
    p = nl + 1;
    if (!strncmp(line, "utc,", 4)) continue;             // the header
    char* f[9];
    int n = 0;
    char* q = line;
    while (n < 9) { f[n++] = q; char* c = strchr(q, ','); if (!c) break; *c = 0; q = c + 1; }
    if (n < 9 || !numeric(f[0]) || !numeric(f[1]) || !numeric(f[2]) || !numeric(f[5]) || !numeric(f[6]) || !numeric(f[7])) continue;
    safeText(f[3]); safeText(f[4]); safeText(f[8]);
    if (bodyLen + 200 >= sizeof(body) - 4) { used -= lineBytes; break; }
    bodyLen += snprintf(body + bodyLen, sizeof(body) - bodyLen,
                        "%s{\"type\":\"%s\",\"lat\":%s,\"lon\":%s,\"timestamp\":%s,\"node\":\"%s\",\"hops\":%s,\"rssi\":%s,\"snr\":%s,\"extra\":\"%s\"}",
                        firstOut ? "" : ",", f[3], f[1], f[2], f[0], f[4], f[5], f[6], f[7], f[8]);
    firstOut = false;
    rows++;
  }
  batchBytes = used;                                     // also covers header and rows skipped as malformed
  batchRows = rows;
  if (rows == 0) return false;
  bodyLen += snprintf(body + bodyLen, sizeof(body) - bodyLen, "]}");
  return true;
}

// ── the network task: WiFi and HTTPS only, never the card ──
static void endTask(Result r) {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  result = r;
  phase = PH_END;
  taskAlive = false;
  vTaskDelete(nullptr);
}

static bool urlAllowed(const char* u) {
  if (!strncmp(u, "https://", 8)) return true;
#ifdef TDOS_COV_ALLOW_HTTP
  if (!strncmp(u, "http://", 7)) return true;             // bench testing against a receiver on the LAN
#endif
  return false;
}

static void syncTask(void*) {
  const char* ssid = cfg().wifiSsid;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  int n = WiFi.scanNetworks(false, true);                // is home in range? a scan is cheap, a failed join is not
  bool seen = false;
  for (int i = 0; i < n; i++) if (WiFi.SSID(i) == ssid) seen = true;
  WiFi.scanDelete();
  if (!seen) endTask(RES_NOWIFI);
  WiFi.begin(ssid, cfg().wifiPass);
  for (int i = 0; i < 150 && WiFi.status() != WL_CONNECTED; i++) delay(100);
  if (WiFi.status() != WL_CONNECTED) endTask(RES_CONNECT);
  if (!strncmp(cfg().covUrl, "https://", 8) && time(nullptr) < 1700000000) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    for (int i = 0; i < 100 && time(nullptr) < 1700000000; i++) delay(100);
    if (time(nullptr) < 1700000000) endTask(RES_TIME);
  }
  phase = PH_READY;
  for (;;) {
    while (cmd == CMD_NONE) delay(20);
    if (cmd == CMD_FINISH) endTask(RES_OK);
    cmd = CMD_NONE;
    phase = PH_POSTING;
    HTTPClient http;
    WiFiClientSecure secure;
    WiFiClient plain;
    bool tls = !strncmp(cfg().covUrl, "https://", 8);
    if (tls) secure.setCACertBundle(CA_BUNDLE);
    http.setTimeout(12000);
    http.setUserAgent("tdeck-os/" TDOS_VERSION);
    bool ok = tls ? http.begin(secure, cfg().covUrl) : http.begin(plain, cfg().covUrl);
    int code = -1;
    if (ok) {
      http.addHeader("Content-Type", "application/json");
      http.addHeader("X-API-Key", cfg().covKey);
      code = http.POST((uint8_t*)body, bodyLen);
      http.end();
    }
    if (code <= 0) {
      char tlsErr[100] = "";
      if (tls) secure.lastError(tlsErr, sizeof(tlsErr));
      Serial.printf("[cov] post failed: begin=%d code=%d (%s) tls='%s' maxalloc=%u\n", (int)ok, code, HTTPClient::errorToString(code).c_str(), tlsErr, (unsigned)ESP.getMaxAllocHeap());
    }
    httpCode = code;
    phase = PH_POSTED;
    while (phase == PH_POSTED) delay(20);                // the main loop reads the answer, then sets READY or asks to finish
  }
}

static void startTask() {
  taskAlive = true;
  phase = PH_CONNECTING;
  cmd = CMD_NONE;
  xTaskCreatePinnedToCore(syncTask, "covsync", 10240, nullptr, 1, nullptr, 0);
}

// ── the state machine, in the main loop ──
static void setStatus() {
  uint32_t ago = (millis() - lastEndMs) / 60000;
  char when[12];
  if (ago < 1) snprintf(when, sizeof(when), "now"); else if (ago < 120) snprintf(when, sizeof(when), "%lum", (unsigned long)ago); else snprintf(when, sizeof(when), "%luh", (unsigned long)(ago / 60));
  switch (lastResult) {            // short: it sits on one Settings row
    case RES_OK: if (lastRows) snprintf(status, sizeof(status), "Sent %lu, %s ago", (unsigned long)lastRows, when); else snprintf(status, sizeof(status), "Up to date"); break;
    case RES_NOWIFI: snprintf(status, sizeof(status), "Not at home WiFi"); break;
    case RES_CONNECT: snprintf(status, sizeof(status), "WiFi join failed"); break;
    case RES_TIME: snprintf(status, sizeof(status), "No network time"); break;
    case RES_HTTP:
      if (lastHttp > 0) snprintf(status, sizeof(status), "Receiver said %d", lastHttp);
      else snprintf(status, sizeof(status), "Receiver unreachable");
      break;
    case RES_CONFIG: snprintf(status, sizeof(status), "Set address and key"); break;
    case RES_MEMORY: snprintf(status, sizeof(status), "Low memory"); break;
    default: snprintf(status, sizeof(status), "Not tried yet"); break;
  }
}

const char* covsync_status() {
  if (phase != PH_IDLE) { snprintf(status, sizeof(status), "Uploading..."); return status; }
  setStatus();
  return status;
}

bool covsync_now() {
  if (phase != PH_IDLE || updater_busy()) return false;
  if (!cfg().covUrl[0] || !cfg().covKey[0] || !urlAllowed(cfg().covUrl) || !cfg().wifiSsid[0]) { lastResult = RES_CONFIG; return false; }
  wantNow = true;
  return true;
}

void covsync_tick(uint32_t now, bool parked, bool lowBattery) {
  if (parked) { if (!parkedSinceMs) parkedSinceMs = now; } else parkedSinceMs = 0;

  switch (phase) {
    case PH_IDLE: {
      if (taskAlive) return;
      const Settings& c = cfg();
      bool due = now >= nextAttemptMs && cfg().covUpload && parkedSinceMs && now - parkedSinceMs >= PARKED_FOR_MS && !lowBattery;
      if (!wantNow && !due) return;
      if (!c.covUrl[0] || !c.covKey[0] || !c.wifiSsid[0] || !urlAllowed(c.covUrl)) { wantNow = false; nextAttemptMs = now + RETRY_ERR_MS; lastResult = RES_CONFIG; return; }
      if (updater_busy()) return;
      sentOffset = storage_coverage_sent();
      uint32_t size = storage_coverage_size();
      if (size < sentOffset) { sentOffset = 0; storage_coverage_set_sent(0); }   // the log was cleared or rotated
      if (size <= sentOffset) { wantNow = false; nextAttemptMs = now + RETRY_OK_MS; return; }      // nothing to send: stay off WiFi
      if (ESP.getMaxAllocHeap() < MIN_BLOCK) { Serial.printf("[cov] low memory: largest block %u\n", (unsigned)ESP.getMaxAllocHeap()); wantNow = false; nextAttemptMs = now + RETRY_ERR_MS; lastResult = RES_MEMORY; lastEndMs = now; return; }
      wantNow = false;
      batchesDone = 0;
      rowsSent = 0;
      failed = false;
      startTask();
      return;
    }
    case PH_READY: {
      if (batchesDone >= MAX_BATCHES || !buildBatch()) { cmd = CMD_FINISH; phase = PH_FINISHING; return; }
      cmd = CMD_POST;
      phase = PH_POSTING;                   // the batch is built: do not build it again while the task picks it up
      return;
    }
    case PH_POSTED: {
      int code = httpCode;
      if (code >= 200 && code < 300) {
        sentOffset += batchBytes;
        storage_coverage_set_sent(sentOffset);
        rowsSent += batchRows;
        batchesDone++;
        phase = PH_READY;
      } else {
        failed = true;
        lastHttp = code;
        cmd = CMD_FINISH;
        phase = PH_FINISHING;
      }
      return;
    }
    case PH_END: {
      lastResult = failed ? RES_HTTP : result;
      lastRows = rowsSent;
      lastEndMs = now;
      nextAttemptMs = now + (lastResult == RES_OK ? RETRY_OK_MS : lastResult == RES_NOWIFI ? RETRY_NOWIFI_MS : RETRY_ERR_MS);
      cmd = CMD_NONE;
      phase = PH_IDLE;
      return;
    }
    default: return;
  }
}
