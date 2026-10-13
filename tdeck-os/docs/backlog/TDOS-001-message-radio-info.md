# TDOS-001: Show hops, SNR and RSSI on received messages

**Type:** feature  **Status:** done  **Area:** Messages, message store, mesh glue

## Why
When range-testing it is not enough to know that a message arrived. You want to know how it got
here: how many repeaters it passed through, and how good the last radio link was. Other MeshCore
apps show this beside each message.

## What is available (verified in MeshCore source)
In `onMessageRecv` and `onChannelMessageRecv` we are handed the `mesh::Packet`:

| Field | Meaning |
|---|---|
| `pkt->getPathHashCount()` | number of repeaters the packet passed through (0 = direct) |
| `pkt->isRouteDirect()` / `isRouteFlood()` | how it was routed |
| `pkt->getSNR()` | SNR of the last hop, in dB (stored as quarter dB) |
| `radio_driver.getLastRSSI()` | RSSI of the last hop in dBm, valid inside the callback |

SNR and RSSI describe only the **final radio link** (the node we heard it from), not the whole route.
The label should say so, so nobody reads it as end-to-end quality.

## Scope
1. Capture hops, route type, SNR and RSSI when a message is received.
2. Store them with the message (see "Storage").
3. Show them on the small line under each incoming bubble, e.g. `9:22 AM  3 hops  SNR 7.5`
   or `9:22 AM  direct  SNR 11.0`.

## Acceptance criteria
- Every new incoming DM and channel message shows hops (or "direct") and SNR under the bubble.
- Values survive a reboot (they come back from the SD card log).
- Messages stored before this change still display, without the radio info and without an error.
- Outgoing messages are unchanged.

## Technical notes
- `Msg` gains `int8_t hops` (-1 unknown), `int8_t snr4`, `int16_t rssi`, `uint8_t route`.
- The SD log record `'M'` has a fixed header followed by text. Add a new record type `'N'` (same
  layout plus the new fields) rather than changing `'M'`, so old logs still load.
- The `MsgSink::onAdd` signature already receives the whole `Msg`; the sink needs no change.
- RSSI must be read inside the callback: the radio's "last RSSI" is overwritten by the next packet.

## Open questions
- Show RSSI as well as SNR on the line, or keep RSSI for the details sheet (TDOS-002)? Space is tight.
