#include "updater.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <mbedtls/sha256.h>
#include "ed_25519.h"
#include "update_key.h"
#include "ca_bundle.h"
#include <time.h>
#include "version.h"
#include "settings.h"

#ifndef TDOS_UPDATE_URL
#define TDOS_UPDATE_URL "https://github.com/tonyperkins/tberry/releases/latest/download/manifest.json"
#endif

static UpStatus st = {};
static volatile bool cancelReq = false;
static volatile bool busy = false;

// ── scan ─────────────────────────────────────────────────────────
static constexpr int MAX_NETS = 12;
struct Net { char ssid[33]; int rssi; };
static Net nets[MAX_NETS];
static volatile int netCount = 0;
static volatile bool scanDone = false;

static void scanTask(void*) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  int n = WiFi.scanNetworks(false, false);
  int c = 0;
  for (int i = 0; i < n && c < MAX_NETS; i++) {          // strongest first; skip hidden and repeats
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    bool dup = false;
    for (int k = 0; k < c; k++) if (s == nets[k].ssid) dup = true;
    if (dup) continue;
    snprintf(nets[c].ssid, sizeof(nets[c].ssid), "%s", s.c_str());
    nets[c].rssi = WiFi.RSSI(i);
    c++;
  }
  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);
  netCount = c;
  scanDone = true;
  busy = false;
  vTaskDelete(nullptr);
}

void updater_scan_start() {
  if (busy) return;
  busy = true;
  scanDone = false;
  netCount = 0;
  xTaskCreatePinnedToCore(scanTask, "wifiscan", 6144, nullptr, 1, nullptr, 0);
}
bool updater_scan_done() { return scanDone; }
int updater_scan_count() { return netCount; }
const char* updater_scan_ssid(int i) { return (i >= 0 && i < netCount) ? nets[i].ssid : ""; }
int updater_scan_rssi(int i) { return (i >= 0 && i < netCount) ? nets[i].rssi : -100; }

// ── manifest ─────────────────────────────────────────────────────
struct Manifest {
  char version[16];
  uint32_t build, size;
  uint8_t sha256[32];
  uint8_t sig[64];
  char url[200];
};
static Manifest man;

// Finds "key": in a small JSON document; copies a string value or reads a number.
static bool jsonString(const char* doc, const char* key, char* out, size_t n) {
  char pat[24];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char* p = strstr(doc, pat);
  if (!p) return false;
  p = strchr(p + strlen(pat), ':');
  if (!p) return false;
  p = strchr(p, '"');
  if (!p) return false;
  const char* e = strchr(p + 1, '"');
  if (!e || (size_t)(e - p - 1) >= n) return false;
  memcpy(out, p + 1, e - p - 1);
  out[e - p - 1] = 0;
  return true;
}
static bool jsonNumber(const char* doc, const char* key, uint32_t* out) {
  char pat[24];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char* p = strstr(doc, pat);
  if (!p) return false;
  p = strchr(p + strlen(pat), ':');
  if (!p) return false;
  *out = (uint32_t)strtoul(p + 1, nullptr, 10);
  return true;
}
static bool fromHex(const char* hex, uint8_t* out, size_t n) {
  if (strlen(hex) != n * 2) return false;
  for (size_t i = 0; i < n; i++) {
    char b[3] = { hex[2 * i], hex[2 * i + 1], 0 };
    char* end;
    out[i] = (uint8_t)strtoul(b, &end, 16);
    if (*end) return false;
  }
  return true;
}
static bool parseManifest(const char* doc) {
  char hex[140];
  memset(&man, 0, sizeof(man));
  if (!jsonString(doc, "version", man.version, sizeof(man.version))) return false;
  if (!jsonNumber(doc, "build", &man.build) || !jsonNumber(doc, "size", &man.size)) return false;
  if (!jsonString(doc, "sha256", hex, sizeof(hex)) || !fromHex(hex, man.sha256, 32)) return false;
  if (!jsonString(doc, "sig", hex, sizeof(hex)) || !fromHex(hex, man.sig, 64)) return false;
  if (!jsonString(doc, "url", man.url, sizeof(man.url))) return false;
  return man.size > 100000 && man.size < 6000000;
}

// ── connection ───────────────────────────────────────────────────
static void fail(const char* m) {
  snprintf(st.msg, sizeof(st.msg), "%s", m);
  st.state = UP_ERROR;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  busy = false;
}

static bool connectWifi() {
  st.state = UP_CONNECTING;
  if (!cfg().wifiSsid[0]) { fail("No WiFi network saved"); return false; }
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg().wifiSsid, cfg().wifiPass);
  for (int i = 0; i < 300 && WiFi.status() != WL_CONNECTED; i++) {      // up to 30 s
    if (cancelReq) { fail("Cancelled"); return false; }
    delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) { fail("Could not join WiFi (check the password)"); return false; }
  if (time(nullptr) < 1700000000) {                 // certificates carry dates: get the time from the network
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    for (int i = 0; i < 100 && time(nullptr) < 1700000000; i++) {
      if (cancelReq) { fail("Cancelled"); return false; }
      delay(100);
    }
    if (time(nullptr) < 1700000000) { fail("Could not get the time from the network"); return false; }
  }
  return true;
}

static bool allowedUrl(const char* url) {
  if (strncmp(url, "https://", 8) == 0) return true;
#ifdef TDOS_UPDATE_ALLOW_HTTP
  if (strncmp(url, "http://", 7) == 0) return true;            // bench testing only, never in a release
#endif
  return false;
}

// Opens `url`, following redirects (GitHub serves release files from a CDN host).
static bool openUrl(HTTPClient& http, WiFiClient*& client, const char* url) {
  static WiFiClientSecure secure;
  static WiFiClient plain;
  if (strncmp(url, "https://", 8) == 0) {
    secure.setCACertBundle(CA_BUNDLE);
    client = &secure;
  } else {
    client = &plain;
  }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  http.setUserAgent("tdeck-os/" TDOS_VERSION);
  return http.begin(*client, url);
}

static volatile bool installReq = false;
static void doInstall();

static void checkTask(void*) {
  cancelReq = false;
  memset(st.msg, 0, sizeof(st.msg));
  if (!connectWifi()) { vTaskDelete(nullptr); return; }
  st.state = UP_CHECKING;
  const char* url = TDOS_UPDATE_URL;
  if (!allowedUrl(url)) { fail("Update address not allowed"); vTaskDelete(nullptr); return; }
  HTTPClient http;
  WiFiClient* client = nullptr;
  if (!openUrl(http, client, url)) { fail("Could not start the request"); vTaskDelete(nullptr); return; }
  int code = http.GET();
  if (code != 200) {
    char m[40];
    snprintf(m, sizeof(m), code > 0 ? "Server answered %d" : "Could not reach the server (%d)", code);
    http.end();
    fail(m);
    vTaskDelete(nullptr);
    return;
  }
  // The manifest is tiny: read at most 2 KB, so a wrong address cannot make us buffer a big file.
  static char body[2048];
  size_t blen = 0;
  {
    WiFiClient* bs = http.getStreamPtr();
    uint32_t t0 = millis();
    while (blen < sizeof(body) - 1 && millis() - t0 < 8000 && (http.connected() || bs->available())) {
      int n = bs->available();
      if (n <= 0) { delay(5); continue; }
      int got = bs->readBytes(body + blen, min<size_t>(n, sizeof(body) - 1 - blen));
      if (got > 0) blen += got;
    }
    body[blen] = 0;
  }
  bool tooBig = blen >= sizeof(body) - 1;
  http.end();
  if (tooBig || !parseManifest(body)) { fail("Update information is not valid"); vTaskDelete(nullptr); return; }
  snprintf(st.version, sizeof(st.version), "%s", man.version);
  st.size = man.size;
  st.done = 0;
  if (man.build <= TDOS_BUILD) {
    st.state = UP_UPTODATE;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  } else {
    st.state = UP_AVAILABLE;                 // WiFi stays up for the install
    installReq = false;
    while (!installReq && !cancelReq) delay(50);
    if (installReq && !cancelReq) { doInstall(); vTaskDelete(nullptr); return; }
    WiFi.disconnect(true);                   // the user left the screen
    WiFi.mode(WIFI_OFF);
    st = {};
  }
  busy = false;
  vTaskDelete(nullptr);
}

void updater_check_start() {
  if (busy) return;
  busy = true;
  st = {};
  if (xTaskCreatePinnedToCore(checkTask, "upcheck", 12288, nullptr, 1, nullptr, 0) != pdPASS) {
    snprintf(st.msg, sizeof(st.msg), "Not enough memory");
    st.state = UP_ERROR;
    busy = false;
  }
}

// ── install ──────────────────────────────────────────────────────
// Runs inside the check task: the device has only ~34 KB of heap free while WiFi is up, too little to start a second 12 KB task.
static void doInstall() {
  Serial.println("[upd] installing");
  if (!allowedUrl(man.url)) { fail("Download address not allowed"); return; }
  HTTPClient http;
  WiFiClient* client = nullptr;
  if (!openUrl(http, client, man.url)) { fail("Could not start the download"); return; }
  st.state = UP_DOWNLOADING;
  st.done = 0;
  int code = http.GET();
  if (code != 200) { http.end(); fail("Download failed"); return; }
  int len = http.getSize();
  if (len > 0 && (uint32_t)len != man.size) { http.end(); fail("Size does not match"); return; }
  if (!Update.begin(man.size, U_FLASH)) { http.end(); fail("No room for the update"); return; }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  WiFiClient* s = http.getStreamPtr();
  static uint8_t buf[2048];
  uint32_t got = 0, lastData = millis();
  bool ok = true;
  while (got < man.size) {
    if (cancelReq) { ok = false; snprintf(st.msg, sizeof(st.msg), "Cancelled"); break; }
    int avail = s->available();
    if (avail <= 0) {
      if (!http.connected() && !s->available()) { ok = false; snprintf(st.msg, sizeof(st.msg), "Connection lost"); break; }
      if (millis() - lastData > 20000) { ok = false; snprintf(st.msg, sizeof(st.msg), "Download stalled"); break; }
      delay(5);
      continue;
    }
    int n = s->readBytes(buf, min<uint32_t>(sizeof(buf), min<uint32_t>(avail, man.size - got)));
    if (n <= 0) continue;
    lastData = millis();
    mbedtls_sha256_update(&sha, buf, n);
    if (Update.write(buf, n) != (size_t)n) { ok = false; snprintf(st.msg, sizeof(st.msg), "Could not write to flash"); break; }
    got += n;
    st.done = got;
  }
  http.end();
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  if (!ok) { Update.abort(); st.state = UP_ERROR; WiFi.disconnect(true); WiFi.mode(WIFI_OFF); busy = false; return; }

  st.state = UP_VERIFYING;
  uint8_t msg[9 + 4 + 4 + 32];
  memcpy(msg, "TBERRYFW1", 9);
  memcpy(msg + 9, &man.build, 4);
  memcpy(msg + 13, &man.size, 4);
  memcpy(msg + 17, digest, 32);
  if (memcmp(digest, man.sha256, 32) != 0) { Update.abort(); fail("Checksum does not match"); return; }
  if (!ed25519_verify(man.sig, msg, sizeof(msg), UPDATE_PUBLIC_KEY)) { Update.abort(); fail("Signature is not valid"); return; }
  if (!Update.end(false) || !Update.isFinished()) { fail("Could not finish the update"); return; }
  st.state = UP_DONE;                         // the UI restarts the device
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  busy = false;
}

void updater_install_start() {
  Serial.printf("[upd] install requested, state=%d\n", (int)st.state);
  if (st.state != UP_AVAILABLE) return;
  installReq = true;
}

void updater_stop() {
  cancelReq = true;
  if (!busy) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    if (st.state != UP_DONE) st = {};
  }
}

const UpStatus& updater_status() { return st; }
bool updater_busy() { return busy; }
