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
4. The setting is saved and applied at boot, **after a start-up time window** (next section).
5. Switching back on shows "Searching" (amber, pulsing) until a fix returns. Do not play the lock chime
   for a fix that was only ever interrupted for a few seconds.

## Start-up time window (decided)
This device has **no real-time clock chip** (confirmed by an I2C scan: only the touch controller,
keyboard and audio chip are present), so after every power cycle the clock is unset and the screen
shows no time until GPS or the mesh provides one. The GPS is the quickest reliable source, so:

1. **At every boot the GPS is powered and read regardless of the on/off setting.**
2. The window **ends when GPS time is obtained, or after a timeout, whichever comes first**.
   Suggested timeout: 120 seconds (a cold start usually has the time within 30 to 60 seconds with a
   clear sky; indoors it may never arrive, and we should not keep a receiver running for that).
3. When the window ends, **the setting is respected**: if GPS is off, the module goes to standby and
   its data is ignored from then on.
4. Mesh time received during the window does not end it early (another node's clock can be wrong),
   but if GPS time never arrives the mesh time simply stays in use.
5. While the window is open the top-bar icon shows its normal "searching" pulse even though the
   setting is off, so the user can see why the receiver is running. The Status screen says
   "start-up time window, N s left".
6. With GPS **on**, nothing changes: it simply keeps running.
7. Until a time is available the lock screen and status bar show `--:--`. No guessed or stored time
   is shown (decided: no save-and-restore of the last known time).

## Acceptance criteria
- Turning GPS off from the panel takes one tap and is reflected at once in the top bar, Status and Map.
- Battery use is measurably lower with GPS off (measure over 30 minutes at the same brightness, or
  read the current if a meter is available). If it is not, the module is not really sleeping.
- After a reboot with GPS off, the module is on for at most the window, then goes back to standby.
- With GPS off and a clear sky, the clock is set by GPS within the window and the module then sleeps.
- With GPS off and no sky view, the window times out at the configured length and the module sleeps.
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
1. Should the window also end as soon as GPS time arrives (as written, for power), or always run the
   full timeout so a position fix has time to settle? Ending early is better for battery, and
   position is wanted only when GPS is on.
2. A timed "GPS on for 5 minutes" mode for battery-conscious use: worth it, or too much?
3. Timeout length: 120 s is a guess. The test is the cold-start time on the real device.

## Resolved
- Turning GPS off also stops GPS time sync after the window; the clock then falls back to the mesh.
