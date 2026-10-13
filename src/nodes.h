#pragma once
// Everyone this device has heard on the mesh, with the signal of the last packet from them.
// Fed by the mesh callbacks, read by the Nodes screen.
#include <stdint.h>

struct NodeInfo {
  uint8_t key[4];       // first 4 bytes of the public key
  char name[32];
  uint8_t type;         // ADV_TYPE_*: 1 chat, 2 repeater, 3 room, 4 sensor
  uint32_t lastTs;      // unix seconds when last heard (0 = restored from storage, time unknown)
  int16_t rssi;         // dBm of the last packet; 0 = not measured yet
  int8_t snr4;          // SNR in quarter dB
  int8_t hops;          // path length of the last packet; -1 = unknown
  int32_t lat, lon;     // advertised position in micro-degrees; 0, 0 = none
};

static constexpr int MAX_NODES = 64;

void nodes_seen(const uint8_t* key4, const char* name, uint8_t type, uint32_t ts, int rssi, float snr, int hops,
                int32_t lat = 0, int32_t lon = 0);
int nodes_count();
const NodeInfo* nodes_sorted(int rank);     // rank 0 = heard most recently
uint32_t nodes_version();
