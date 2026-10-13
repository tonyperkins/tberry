# TDOS-002: Message details sheet

**Type:** feature  **Status:** open  **Area:** Messages  **Depends on:** TDOS-001

## Why
The inline line (TDOS-001) is deliberately short. A details view gives the full story of one
message, and is the natural home for the "show on map" action (TDOS-004).

## Interaction
Long-press a message bubble. A sheet slides up with the fields below and a Close button. (Tap is
reserved for the reply mention, TDOS-003.)

## Fields
Everything marked * needs new capture work; the rest comes from TDOS-001.

| Field | Source |
|---|---|
| Sender name and key prefix | contact / parsed channel text |
| Received time, sender's timestamp, **delay or clock skew** between them * | `onMessageRecv(... sender_timestamp ...)` vs our RTC |
| Route: flood or direct, hops, SNR, RSSI | TDOS-001 |
| **Path: the repeaters it passed through, by name** * | `pkt->path[]` hashes matched against known nodes' keys |
| Payload size in bytes, estimated airtime * | payload length, RadioLib time-on-air |
| **Times heard** (duplicate copies of a flood) * | count in the mesh tables before the duplicate is dropped |
| For sent messages: delivery round trip, retries, hops of the route used | already stored (`rtt100`) |
| Channel name for channel messages | conversation |

## Acceptance criteria
- Long-press on any bubble opens the sheet; Back, a tap outside, or Close dismisses it.
- Fields that are unknown show a dash, never a wrong value.
- The sheet fits 320x240 without scrolling for the common fields, and scrolls for the rest.

## Technical notes
- Path hashes are 1 to 3 bytes each (`getPathHashSize()`); matching a hash to a repeater name is a
  prefix match on public keys we know, and can be ambiguous. Show "?" or both candidates, and say so.
- "Times heard" needs a hook where MeshCore drops duplicates (`SimpleMeshTables::hasSeen`).
  Subclass it in tdeck-os to count, keyed by packet hash, in a small LRU.
- The path is only stored per message if we keep up to 8 hashes with it (about 24 bytes): decide
  whether to keep it on SD only or in RAM too.

## Open questions
- You mentioned metrics seen in other apps. Which ones did you want? I have listed the ones I know of
  from MeshCore and MeshDesk; please add or strike any.
