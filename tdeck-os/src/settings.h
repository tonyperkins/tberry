#pragma once
// Persistent user settings. Stored as one small file in flash and saved a couple of seconds
// after the last change, so changing a setting never stalls the main loop.
#include <stdint.h>
#include <FS.h>

static constexpr int SOUND_MAX = 16;

struct Settings {
  uint32_t magic;          // identifies a valid settings file
  uint16_t version;
  char name[32];           // node name; empty = generate one from the node's key
  int16_t tzMinutes;       // standard-time offset from UTC, in minutes
  uint8_t dst;             // 1 = add an hour
  uint8_t brightness;      // backlight level 1..16 (the T-Deck backlight chip has 16 steps)
  uint8_t pinSet;          // 0 until the owner has chosen a PIN
  char pin[8];             // 4 digits
  float freq;              // MHz
  float bw;                // kHz
  uint8_t sf;              // spreading factor
  uint8_t cr;              // coding rate denominator (5..8)
  int8_t txPower;          // dBm
  uint8_t volume;          // 0 (muted) to 100
  uint8_t channelSound;    // 0 never, 1 only when mentioned (default), 2 every channel message
  uint8_t gpsEnabled;      // 0 off after the start-up time window, 1 always on (default)
  uint8_t setupDone;       // 0 until the first-boot setup has been completed
  uint8_t screenOff;       // index into the screen-off list in ui.cpp: time on the clock before the backlight goes off
  uint8_t screenTimeout;   // index into the timeout list in ui.cpp: 30 s, 1 min (default), 2, 5, 10 min, never
  char wifiSsid[33];       // network used for updates; empty = none saved
  char wifiPass[64];
  uint8_t kbLight;         // key backlight level 0 (off) to 4; lit only while the screen is on
  uint8_t covOn;           // 1 = coverage logging and probes (default); the pull-down switch
  uint8_t covDisc;         // index into COV_DISC_SECS: zero-hop discovery probe interval (default 1 minute)
  uint8_t covDm;           // index into COV_DM_MINUTES: direct-message probe to the target (default 30 minutes)
  uint8_t covTargetSet;    // 1 once a probe target has been chosen; probes to the owner are off until then
  uint8_t covTarget[4];    // first bytes of the target contact's public key
  uint8_t covUpload;       // 1 = upload the coverage log over WiFi when at home (needs an address and key)
  char covUrl[96];         // receiver address, https://host/api/coverage
  char covKey[40];         // key sent as X-API-Key
  uint8_t soundN;          // per-conversation sound overrides in use
  uint8_t soundKey[SOUND_MAX][4];    // the conversation (first bytes of its key)
  uint8_t soundState[SOUND_MAX];     // 1 silent, 2 chime on every message
};

void settings_begin(fs::FS& fs);       // loads, or applies defaults
Settings& cfg();
void settings_changed();               // mark dirty; saved shortly after
void settings_tick();                  // call from the main loop
void settings_defaults(bool keepName); // restore factory values

// Per-conversation sound: 0 follows the defaults (direct messages chime, channels follow the channel
// policy), 1 silent, 2 chime on every message.
uint8_t sound_override(const uint8_t key[4]);
bool sound_set(const uint8_t key[4], uint8_t state);   // false when the list is full
