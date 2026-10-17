#pragma once
// Over-the-air update: join the saved WiFi network, fetch a small signed manifest, and install the
// firmware it points to. Everything runs in a background task; the UI polls updater_status().
//
// Trust: the manifest carries an Ed25519 signature over (build, size, SHA-256 of the image) made
// with the key from tools/sign_release.py. The device hashes the image while writing it and only
// switches to it when the signature, size and hash all match.
#include <stdint.h>

enum UpState {
  UP_IDLE,
  UP_CONNECTING,     // joining WiFi
  UP_CHECKING,       // fetching the manifest
  UP_UPTODATE,
  UP_AVAILABLE,      // newer version found: ask before installing
  UP_DOWNLOADING,
  UP_VERIFYING,
  UP_DONE,           // installed; the device restarts next
  UP_ERROR
};

struct UpStatus {
  UpState state;
  char msg[72];          // error text or short detail
  char version[16];      // version offered by the manifest
  uint32_t size;         // bytes in the new image
  uint32_t done;         // bytes written so far
};

// Scan for networks (background). Results stay until the next scan.
void updater_scan_start();
bool updater_scan_done();
int updater_scan_count();
const char* updater_scan_ssid(int i);
int updater_scan_rssi(int i);

void updater_check_start();            // connect and look for an update
void updater_install_start();          // after UP_AVAILABLE
bool updater_busy();                   // a scan, check or install is running (WiFi is in use)
void updater_stop();                   // cancel what is running, switch WiFi off
const UpStatus& updater_status();
