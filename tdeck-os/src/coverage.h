#pragma once
// Coverage logging: where the mesh can be heard, and whether it can reach the owner.
//
// Everything goes to /tdos/coverage.csv on the SD card, one row per observation:
//   utc,lat,lon,type,node,hops,rssi,snr,extra
// Types: RX (a packet heard, passive), DISC (a repeater answered a discovery probe), DM (a direct message
// probe to the owner: delivered or not), STAT (airtime counters, so the cost of probing can be measured).
// Passive rows are binned into ~100 m cells and only the best reading per cell and relay is kept,
// so the file grows with the area covered, not with the time spent driving.
#include <stdint.h>

void cov_begin();
void cov_rx(uint32_t utc, int32_t latE6, int32_t lonE6, int hops, uint32_t lastHop, uint8_t hopBytes, int rssi, float snr);
void cov_disc(uint32_t utc, int32_t latE6, int32_t lonE6, const uint8_t* key, float upSnr, int rssi, float snr);
void cov_dm(uint32_t utc, int32_t latE6, int32_t lonE6, bool ok, uint32_t rttMs, int hops, int rssi, float snr);
void cov_stat(uint32_t utc, int32_t latE6, int32_t lonE6, uint32_t airMs, uint32_t tx, uint32_t rx);
void cov_tick();                    // call from the loop; writes the buffered rows now and then
uint32_t cov_rows();                // rows logged since start

// Settings lists. Index 0 is always Off.
constexpr int COV_DISC_N = 5;
extern const uint16_t COV_DISC_SECS[COV_DISC_N];
extern const char* const COV_DISC_NAMES[COV_DISC_N];
constexpr int COV_DM_N = 6;
extern const uint16_t COV_DM_MINUTES[COV_DM_N];
extern const char* const COV_DM_NAMES[COV_DM_N];
