#include "coverage.h"
#include <Arduino.h>
#include "storage.h"

const uint16_t COV_DISC_SECS[COV_DISC_N] = { 0, 30, 60, 120, 300 };
const char* const COV_DISC_NAMES[COV_DISC_N] = { "Off", "30 seconds", "1 minute", "2 minutes", "5 minutes" };
const uint16_t COV_DM_MINUTES[COV_DM_N] = { 0, 10, 15, 30, 60, 120 };
const char* const COV_DM_NAMES[COV_DM_N] = { "Off", "10 minutes", "15 minutes", "30 minutes", "1 hour", "2 hours" };

static constexpr int32_t CELL_LAT_E6 = 900;      // ~100 m
static constexpr int32_t CELL_LON_E6 = 1040;     // ~100 m at 30 N (Austin); only used to bin
static constexpr int CELLS = 256;
static constexpr size_t BUF_BYTES = 2048;

struct Cell { int32_t la, lo; uint32_t relay; int16_t rssi; bool used; };
static Cell cells[CELLS];
static int nextCell = 0;
static char buf[BUF_BYTES];
static size_t used = 0;
static uint32_t lastFlush = 0;
static uint32_t rows = 0;

void cov_begin() { memset(cells, 0, sizeof(cells)); used = 0; }

static void put(const char* line, size_t n) {
  if (used + n >= BUF_BYTES) return;                // the card is slow or missing: drop, never block
  memcpy(buf + used, line, n);
  used += n;
  rows++;
}

static void row(uint32_t utc, int32_t latE6, int32_t lonE6, const char* type, const char* node, int hops, int rssi, float snr, const char* extra) {
  char l[128];
  int n = snprintf(l, sizeof(l), "%lu,%.6f,%.6f,%s,%s,%d,%d,%.2f,%s\n", (unsigned long)utc, latE6 / 1e6, lonE6 / 1e6,
                   type, node, hops, rssi, snr, extra);
  if (n > 0 && n < (int)sizeof(l)) put(l, n);
}

void cov_rx(uint32_t utc, int32_t latE6, int32_t lonE6, int hops, uint32_t lastHop, uint8_t hopBytes, int rssi, float snr) {
  int32_t la = latE6 / CELL_LAT_E6, lo = lonE6 / CELL_LON_E6;
  Cell* hit = nullptr;
  for (auto& c : cells) if (c.used && c.la == la && c.lo == lo && c.relay == lastHop) { hit = &c; break; }
  if (hit) {
    if (rssi < hit->rssi + 3) return;               // not noticeably better than what this cell already has
    hit->rssi = (int16_t)rssi;
  } else {
    Cell& c = cells[nextCell];
    nextCell = (nextCell + 1) % CELLS;
    c = { la, lo, lastHop, (int16_t)rssi, true };
  }
  char node[10] = "";
  if (hops > 0) snprintf(node, sizeof(node), "%0*lX", hopBytes * 2, (unsigned long)lastHop);   // the relay we heard it from
  row(utc, latE6, lonE6, "RX", node, hops, rssi, snr, "");
}

void cov_disc(uint32_t utc, int32_t latE6, int32_t lonE6, const uint8_t* key, float upSnr, int rssi, float snr) {
  char node[10], extra[16];
  snprintf(node, sizeof(node), "%02X%02X%02X%02X", key[0], key[1], key[2], key[3]);
  snprintf(extra, sizeof(extra), "up=%.2f", upSnr);
  row(utc, latE6, lonE6, "DISC", node, 0, rssi, snr, extra);
}

void cov_dm(uint32_t utc, int32_t latE6, int32_t lonE6, bool ok, uint32_t rttMs, int hops, int rssi, float snr) {
  char extra[24];
  if (ok) snprintf(extra, sizeof(extra), "ok;%lu", (unsigned long)rttMs);
  else snprintf(extra, sizeof(extra), "fail");
  row(utc, latE6, lonE6, "DM", "", hops, ok ? rssi : 0, ok ? snr : 0.0f, extra);
}

void cov_stat(uint32_t utc, int32_t latE6, int32_t lonE6, uint32_t airMs, uint32_t tx, uint32_t rx) {
  char extra[48];
  snprintf(extra, sizeof(extra), "air=%lu;tx=%lu;rx=%lu", (unsigned long)airMs, (unsigned long)tx, (unsigned long)rx);
  row(utc, latE6, lonE6, "STAT", "", 0, 0, 0.0f, extra);
}

void cov_clear() {
  used = 0;
  memset(cells, 0, sizeof(cells));
  rows = 0;
  storage_clear_coverage();
}

void cov_tick() {
  if (!used) return;
  if (millis() - lastFlush < 20000 && used < BUF_BYTES / 2) return;
  lastFlush = millis();
  storage_append_coverage(buf, used);
  used = 0;
}

uint32_t cov_rows() { return rows; }
