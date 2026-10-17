#include "settings.h"
#include <Arduino.h>

static constexpr uint32_t MAGIC = 0x53444F54;   // "TODS"
static constexpr uint16_t VERSION = 12;     // 2 -> 3: volume, 3 -> 4: channel sound policy, 4 -> 5: GPS on/off, 5 -> 6: screen timeout, 6 -> 7: first-boot setup, 7 -> 8: screen off, 8 -> 9: WiFi for updates, 9 -> 10: key backlight, 10 -> 11: coverage, 11 -> 12: coverage upload
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
  s.channelSound = 1;
  s.gpsEnabled = 1;
  s.screenTimeout = 1;
  s.setupDone = 0;
  s.screenOff = 1;
  s.kbLight = 2;
  s.covOn = 1;
  s.covDisc = 2;
  s.covDm = 3;
  s.covUpload = 1;
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
  const Settings defaults = s;
  size_t got = f.read((uint8_t*)&tmp, sizeof(tmp));
  const uint16_t fileVersion = tmp.version;
  if (got >= offsetof(Settings, volume) && tmp.magic == MAGIC) {
    // Older files carry zero in the bytes that later became fields, so take the defaults for those.
    if (tmp.version == 2) { tmp.volume = s.volume; tmp.channelSound = s.channelSound; tmp.gpsEnabled = s.gpsEnabled; tmp.screenTimeout = s.screenTimeout; tmp.setupDone = 1; tmp.screenOff = s.screenOff; tmp.version = VERSION; s = tmp; }
    else if (tmp.version == 3) { tmp.channelSound = s.channelSound; tmp.gpsEnabled = s.gpsEnabled; tmp.screenTimeout = s.screenTimeout; tmp.setupDone = 1; tmp.screenOff = s.screenOff; tmp.version = VERSION; s = tmp; }
    else if (tmp.version == 4) { tmp.gpsEnabled = s.gpsEnabled; tmp.screenTimeout = s.screenTimeout; tmp.setupDone = 1; tmp.screenOff = s.screenOff; tmp.version = VERSION; s = tmp; }
    else if (tmp.version == 5) { tmp.screenTimeout = s.screenTimeout; tmp.setupDone = 1; tmp.screenOff = s.screenOff; tmp.version = VERSION; s = tmp; }
    else if (tmp.version == 6) { tmp.setupDone = 1; tmp.screenOff = s.screenOff; tmp.version = VERSION; s = tmp; }
    else if (tmp.version == 7) { tmp.screenOff = s.screenOff; tmp.version = VERSION; s = tmp; }
    else if (tmp.version == 8) { tmp.kbLight = s.kbLight; tmp.version = VERSION; s = tmp; }   // WiFi fields stay empty
    else if (tmp.version == 9) { tmp.kbLight = s.kbLight; tmp.version = VERSION; s = tmp; }  // the new byte was padding: zero
    else if (tmp.version == 10 || tmp.version == 11) { tmp.version = VERSION; s = tmp; }
    else if (tmp.version == VERSION) s = tmp;
    if (fileVersion < 12 && s.version == VERSION) {            // v12 fields: no address or key, upload on
      s.covUpload = defaults.covUpload; memset(s.covUrl, 0, sizeof(s.covUrl)); memset(s.covKey, 0, sizeof(s.covKey));
    }
    if (fileVersion < 11 && s.version == VERSION) {             // fields added in v11 sit in old padding: zero, so default them
      s.covOn = defaults.covOn; s.covDisc = defaults.covDisc; s.covDm = defaults.covDm; s.covTargetSet = 0; memset(s.covTarget, 0, sizeof(s.covTarget));
    }
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
  s.setupDone = 1;                        // a restore never sends the owner back through setup
  settings_changed();
}
