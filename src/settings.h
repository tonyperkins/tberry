#pragma once
// Persistent user settings. Stored as one small file in flash and saved a couple of seconds
// after the last change, so changing a setting never stalls the main loop.
#include <stdint.h>
#include <FS.h>

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
  uint8_t volume;          // 0 (muted) to 100; added after the first release, so it is the last field
};

void settings_begin(fs::FS& fs);       // loads, or applies defaults
Settings& cfg();
void settings_changed();               // mark dirty; saved shortly after
void settings_tick();                  // call from the main loop
void settings_defaults(bool keepName); // restore factory values
