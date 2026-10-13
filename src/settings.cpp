#include "settings.h"
#include <Arduino.h>

static constexpr uint32_t MAGIC = 0x53444F54;   // "TODS"
static constexpr uint16_t VERSION = 3;     // 2 -> 3: added volume
static constexpr const char* PATH = "/settings";

static Settings s;
static fs::FS* store = nullptr;
static bool dirty = false;
static uint32_t changedAt = 0;

static void applyDefaults() {
  memset(&s, 0, sizeof(s));
  s.magic = MAGIC;
  s.version = VERSION;
  s.tzMinutes = 0;
  s.dst = 0;
  s.brightness = 16;
  s.pinSet = 0;
  s.freq = 910.525f;       // MeshCore US preset
  s.bw = 62.5f;
  s.sf = 7;
  s.cr = 5;
  s.txPower = 22;
  s.volume = 60;
}

void settings_begin(fs::FS& fs) {
  store = &fs;
  applyDefaults();
  if (!fs.exists(PATH)) return;
  File f = fs.open(PATH);
  if (!f) return;
  // Version 2 files predate volume. The new field sits in what used to be padding, so it would
  // read as 0 (muted): migrate by keeping everything else and using the default volume.
  Settings tmp = s;
  size_t got = f.read((uint8_t*)&tmp, sizeof(tmp));
  if (got >= offsetof(Settings, volume) && tmp.magic == MAGIC) {
    if (tmp.version == 2) { tmp.volume = s.volume; tmp.version = VERSION; s = tmp; }
    else if (tmp.version == VERSION) s = tmp;
  }
  f.close();
}

Settings& cfg() { return s; }

void settings_changed() {
  dirty = true;
  changedAt = millis();
}

void settings_tick() {
  if (!dirty || !store || millis() - changedAt < 2000) return;
  File f = store->open(PATH, "w", true);
  if (f) {
    f.write((const uint8_t*)&s, sizeof(s));
    f.close();
  }
  dirty = false;
}

void settings_defaults(bool keepName) {
  char name[32];
  memcpy(name, s.name, sizeof(name));
  applyDefaults();
  if (keepName) memcpy(s.name, name, sizeof(name));
  settings_changed();
}
