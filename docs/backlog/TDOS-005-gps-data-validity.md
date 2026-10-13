# TDOS-005: GPS tiles show impossible values without a real fix

**Type:** bug  **Status:** done  **Area:** Status screen, GPS parser

## Evidence (two photos of the Status screen, same session, device stationary indoors)

| | Photo 1 | Photo 2 |
|---|---|---|
| GPS FIX | 3D fix, updated 0 s ago | "Fix", updated 8 s ago |
| Satellites | 4 used, 8 in view | **0 used**, 8 in view |
| Accuracy | HDOP 4.2, moderate, about 21 m | **HDOP 100.0**, poor, about 500 m |
| Altitude | 332 m (1088 ft) | **0 m** |
| Speed | **3.6 km/h**, heading 215 | **9.3 km/h**, heading 234 |

Speed is clearly wrong: the unit is on a desk. Photo 2 shows a state with no satellites in use that
the screen still labels "Fix", and values that are really "no data" markers (HDOP 99.9/100, altitude 0).

## Causes
- `RMC` status `A` is accepted as a fix on its own, without checking `GGA` quality or satellites used.
- Some modules keep emitting `A` for a few seconds, or when dead-reckoning, with stale position.
- Speed over ground is Doppler-derived and noisy when the geometry is poor (few satellites, HDOP > 2):
  a stationary receiver can report several km/h.
- HDOP 99.99 or 100 is the module's "invalid" value and is shown as if real.

## Proposed rules
1. **Valid fix** = GGA quality > 0 **and** satellites used >= 4 (3D) or the module reports 2D with
   >= 3. Otherwise the state is "Searching" and position, altitude, speed and heading show "-".
2. **HDOP** shown only if 0 < HDOP < 50.
3. **Altitude** shown only with a 3D fix; add "(+/- tens of m)" in the sub-line, since vertical
   error is usually two to three times the horizontal error.
4. **Speed**: require a 3D fix and HDOP < 5; deadband below 4 km/h shown as "stationary";
   light smoothing (average over 3 seconds) so a single noisy sample does not flash up.
5. Clear speed, heading and altitude immediately when the fix is lost, not after the 30 s timeout.
6. Do not play the "GPS locked" sound or draw the "me" marker until rule 1 is true.

## Acceptance criteria
- With no sky view the screen reads "Searching", with dashes, and the top-bar icon pulses amber.
- A stationary device with a good fix shows "stationary", not a speed.
- HDOP and altitude never display their invalid markers.

## Note on the altitude figure
332 m for that position is plausible (the Leander and Cedar Park area sits around 300 to 340 m), but
GPS altitude is the least accurate output of the receiver. At HDOP 4.2 an error of 50 m or more is
normal, so do not treat it as measured elevation.
