# TDOS-009: GPS on/off switch in Settings and the quick panel

**Type:** feature  **Status:** open  **Area:** Settings, quick panel, GPS, power

## Why
The GPS module keeps the receiver running and the device parsing NMEA all the time. Indoors it never
gets a good fix anyway, and it costs battery. There are also times when you do not want a position at
all. A switch you can reach quickly, from the pull-down panel as well as from Settings, solves both.

## Behaviour
1. **Settings**: a "GPS" row with an on/off chip, next to Daylight saving. Default **on**.
2. **Quick panel** (swipe down from the top edge): a fourth button, "GPS", showing its state, that
   toggles in one tap. The panel has room for four buttons if each is about 74 px wide.
3. When **off**:
   - the GPS module is put in its low-power state and its serial data is ignored,
   - the top-bar icon is greyed with a distinct "off" look (not the red cross, which means "module
     is silent" and would read as a fault),
   - the Status GPS tiles show "GPS off",
   - the "me" marker is hidden on the map, and the map stops following,
   - the clock falls back to mesh time (the time source on Status says so).
4. The setting is saved and applied at boot.
5. Switching back on shows "Searching" (amber, pulsing) until a fix returns. Do not play the lock chime
   for a fix that was only ever interrupted for a few seconds.

## Acceptance criteria
- Turning GPS off from the panel takes one tap and is reflected at once in the top bar, Status and Map.
- Battery use is measurably lower with GPS off (measure over 30 minutes at the same brightness, or
  read the current if a meter is available). If it is not, the module is not really sleeping.
- After a reboot with GPS off, the module does not wake up.
- Position is not used anywhere while off (range-test logs record no coordinates, once they have any).

## Technical notes
- **Software only** (stop parsing, hide markers) saves nothing: the module still draws power.
  Real savings need the module in standby. Saitama's source notes "PMTK standby; any UART byte
  wakes it", which suggests an L76K-class module that accepts `$PMTK161,0*28` to sleep. Verify against
  the module's datasheet before relying on it, and check the T-Deck Plus schematic for a power
  enable pin, which would be better still.
- Waking: any byte on the UART (documented for that family), or a full-on command. Keep a short
  hot-start path so a re-enable gets a fix quickly; the module keeps ephemeris in backup RAM only
  while powered, so a hard power-off makes the next fix a cold start (tens of seconds to minutes).
- Settings struct: add a `gpsEnabled` byte and migrate (the next version after 4).
- The map's follow mode and the position field in adverts (when "share my position" exists) must
  both check this flag.

## Open questions
1. Should turning GPS off also stop the time sync from GPS? (As written: yes, time falls back to the
   mesh. Say if you would rather keep GPS time alive and only drop position.)
2. A timed "GPS on for 5 minutes" mode for battery-conscious use: worth it, or too much?
