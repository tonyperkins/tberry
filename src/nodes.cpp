#include "nodes.h"
#include <Arduino.h>

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
