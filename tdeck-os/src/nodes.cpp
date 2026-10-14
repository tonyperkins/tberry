#include "nodes.h"
#include <Arduino.h>
#include <FS.h>

static NodeInfo table[MAX_NODES];
static int count = 0;
static int order[MAX_NODES];
static bool orderDirty = true;
static uint32_t version = 1;

void nodes_seen(const uint8_t* key4, const char* name, uint8_t type, uint32_t ts, int rssi, float snr, int hops,
                int32_t lat, int32_t lon) {
  int i = -1;
  for (int k = 0; k < count; k++) if (memcmp(table[k].key, key4, 4) == 0) { i = k; break; }
  if (i < 0) {
    if (count < MAX_NODES) {
      i = count++;
    } else {                                   // full: replace the one heard longest ago
      i = 0;
      for (int k = 1; k < count; k++) if (table[k].lastTs < table[i].lastTs) i = k;
    }
    memset(&table[i], 0, sizeof(NodeInfo));
    memcpy(table[i].key, key4, 4);
    table[i].hops = -1;
  }
  NodeInfo& n = table[i];
  if (name && *name) { strncpy(n.name, name, sizeof(n.name) - 1); n.name[sizeof(n.name) - 1] = 0; }
  if (type) n.type = type;
  if (ts) n.lastTs = ts;
  if (rssi) { n.rssi = (int16_t)rssi; n.snr4 = (int8_t)constrain((int)lroundf(snr * 4.0f), -128, 127); }
  if (hops >= 0) n.hops = (int8_t)hops;
  if (lat || lon) { n.lat = lat; n.lon = lon; }
  orderDirty = true;
  version++;
}

int nodes_count() { return count; }

const NodeInfo* nodes_sorted(int rank) {
  if (rank < 0 || rank >= count) return nullptr;
  if (orderDirty) {
    for (int i = 0; i < count; i++) order[i] = i;
    for (int a = 1; a < count; a++) {          // insertion sort, newest first
      int v = order[a], b = a - 1;
      while (b >= 0 && table[order[b]].lastTs < table[v].lastTs) { order[b + 1] = order[b]; b--; }
      order[b + 1] = v;
    }
    orderDirty = false;
  }
  return &table[order[rank]];
}

uint32_t nodes_version() { return version; }

const NodeInfo* nodes_find(const uint8_t key4[4]) {
  for (int i = 0; i < count; i++) if (memcmp(table[i].key, key4, 4) == 0) return &table[i];
  return nullptr;
}

double nodes_distance_km(int32_t laE6, int32_t loE6, int32_t lbE6, int32_t lobE6, const char** dir) {
  const double R = 6371.0, D2R = 0.017453292519943295;
  double la1 = laE6 / 1e6 * D2R, lo1 = loE6 / 1e6 * D2R, la2 = lbE6 / 1e6 * D2R, lo2 = lobE6 / 1e6 * D2R;
  double dla = la2 - la1, dlo = lo2 - lo1;
  double a = sin(dla / 2) * sin(dla / 2) + cos(la1) * cos(la2) * sin(dlo / 2) * sin(dlo / 2);
  double d = 2 * R * asin(sqrt(a));
  if (dir) {
    double y = sin(dlo) * cos(la2), x = cos(la1) * sin(la2) - sin(la1) * cos(la2) * cos(dlo);
    double brg = atan2(y, x) / D2R;
    if (brg < 0) brg += 360;
    static const char* const pts[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    *dir = pts[(int)((brg + 22.5) / 45.0) % 8];
  }
  return d;
}

bool nodes_position_for_name(const char* name, int32_t* latE6, int32_t* lonE6) {
  if (!name || !name[0]) return false;
  const NodeInfo* best = nullptr;
  for (int i = 0; i < count; i++) {
    const NodeInfo& n = table[i];
    if (!n.lat && !n.lon) continue;
    if (strncmp(n.name, name, sizeof(n.name)) != 0) continue;
    if (!best || n.lastTs > best->lastTs) best = &n;
  }
  if (!best) return false;
  *latE6 = best->lat; *lonE6 = best->lon;
  return true;
}

bool nodes_position_for_key(const uint8_t key4[4], int32_t* latE6, int32_t* lonE6) {
  for (int i = 0; i < count; i++) {
    if (memcmp(table[i].key, key4, 4) != 0) continue;
    if (!table[i].lat && !table[i].lon) return false;
    *latE6 = table[i].lat; *lonE6 = table[i].lon;
    return true;
  }
  return false;
}

struct __attribute__((packed)) PosRec { uint8_t key[4]; int32_t lat, lon; };

void nodes_positions_save(fs::FS& fs) {
  File f = fs.open("/nodepos", "w", true);
  if (!f) return;
  for (int i = 0; i < count; i++) {
    if (!table[i].lat && !table[i].lon) continue;
    PosRec r;
    memcpy(r.key, table[i].key, 4);
    r.lat = table[i].lat; r.lon = table[i].lon;
    f.write((const uint8_t*)&r, sizeof(r));
  }
  f.close();
}

void nodes_positions_load(fs::FS& fs) {
  if (!fs.exists("/nodepos")) return;
  File f = fs.open("/nodepos");
  if (!f) return;
  PosRec r;
  while (f.read((uint8_t*)&r, sizeof(r)) == sizeof(r)) {
    for (int i = 0; i < count; i++) {
      if (memcmp(table[i].key, r.key, 4) == 0) { table[i].lat = r.lat; table[i].lon = r.lon; break; }
    }
  }
  f.close();
  version++;
}
