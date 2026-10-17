#pragma once
// Uploads the coverage log (/tdos/coverage.csv) to the owner's receiver (receiver/server.py) when the
// device is parked within reach of its saved WiFi network.
//
// The SD card shares its SPI bus with the radio, so every card access happens here in the main loop;
// the background task only does WiFi and HTTPS. A batch is up to 50 rows. The point reached in the file
// is saved after each batch the receiver accepted, so an interrupted upload carries on where it stopped
// and the receiver ignores any row it already has.
#include <stdint.h>

void covsync_begin();                       // after storage and settings: imports /tdos/upload.conf if present
// Call every loop pass. parked: not moving (or no GPS position); lowBattery: skip while below 15% and not charging.
void covsync_tick(uint32_t nowMs, bool parked, bool lowBattery);
bool covsync_now();                         // ask for an attempt right now; false if it cannot start
bool covsync_busy();
const char* covsync_status();               // one line for Settings, e.g. "Uploaded 12 min ago, 120 rows"
uint32_t covsync_pending_bytes();           // how much of the log has not been uploaded
void covsync_set_device(const char* name);  // how this device names itself in the upload
