# TDOS-004: Show a message's origin on the map

**Type:** feature  **Status:** open  **Area:** Messages, Map  **Depends on:** TDOS-002 (for the entry point)

## Why
If a message came from somewhere, seeing where on the map is the quickest way to judge range and
whether a repeater or a person is out of the area.

## What "has a location" means (decided)
**Option A.** A MeshCore message does not carry coordinates. A node that has GPS (or a configured
fixed position) and has "share location" enabled puts its latitude and longitude in its **advert**.
Apps join the two: a message from a node whose advert had a position gets a location marker.

The sample confirms this. Most messages show a small crosshair mark after the hop count
(`8 hops ⌖ · SNR 0.5`); a few do not (`24 hops · SNR 2`, `direct · SNR 11.25`), and those are
nodes that do not advertise a position. Our own node advertises no position (privacy default), so
other people's apps show no crosshair on our messages.

Coordinates typed into message text (option B) are not part of the convention and are out of scope.

## How other apps present it
- The crosshair mark sits on the same line as hops and SNR, and is the control that opens the map.
- Every message line follows `N hops ⌖ · SNR x.xx · 6:22 AM`; one hop is "1 hop", zero is "direct".

## Behaviour
1. A bubble whose origin has a position shows a small pin icon beside its time line.
2. Long-press (TDOS-002 sheet) offers **Show on map**. Tapping the pin icon does the same directly.
3. The map opens centred on that point at zoom 14 to 15, with a highlighted marker, the sender's
   name, and the distance and bearing from this device if we have a GPS fix.
4. Back returns to the same conversation, scrolled to the same message.

## Acceptance criteria
- A message from a node with an advertised position gets the pin; a message from one without does not.
- The map centres correctly and the marker is visible at the default zoom.
- No pin and no crash when the sender cannot be matched to a node.

## Technical notes
- Direct messages identify the contact by key, which is exact. **Channel messages carry only a name**,
  so matching is by name and can be ambiguous when two nodes share one. Prefer the most recently
  heard, and say "approximate" in that case.
- New API in `mapview`: `mapview_focus(lat, lon, label)` that sets zoom, centre and a one-off marker.
- Node positions are not persisted across reboots (contacts are saved without GPS). Either add
  position to the saved contact record or accept that old messages lose the pin until the node is
  heard again. Worth deciding.
- Privacy: this only displays positions other nodes chose to broadcast.

## Open questions
- Should the map also draw the **route** (the repeaters from TDOS-002 that have positions) as a line
  from the sender to us? A natural extension, but see the path-accuracy caveat in TDOS-002.
