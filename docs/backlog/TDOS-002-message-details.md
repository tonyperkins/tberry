# TDOS-002: Message details sheet

**Type:** feature  **Status:** done  **Area:** Messages  **Depends on:** TDOS-001

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

## Notes from the sample
- The apps shown flag a message with a small clock-and-question mark when its **timestamp looks
  unreliable** (the sender's clock disagrees with ours). That is the "delay or clock skew" field
  above, shown as a flag: add it to the inline line when the skew is large (suggest > 10 minutes).
- **Path accuracy is limited.** A path is a list of repeater hashes, and at the default size of one
  byte there are only 256 values for hundreds of repeaters, so several repeaters share each hash.
  Someone in the sample said exactly this ("probably somewhat screwed due to 1 byte stuff").
  Live-map services such as scope.digitaino.com reconstruct routes from reports by many observer
  nodes plus known repeater positions; a single handset cannot disambiguate the same way. We should
  show the path as hashes, name a repeater only when one known repeater matches, and mark
  ambiguous ones. Larger hash sizes (2 or 3 bytes) remove most of the ambiguity, and
  `Packet::getPathHashSize()` tells us which one a packet used.
- Bots on the channel answer "test" messages with the hop count they saw
  (`ACK | Path: 22 hops`, `Copy in Crestview FL ... 11 hops`). That is useful ground truth to compare
  against our own hop count.

## As built
- **Long-press** a bubble to open the sheet (a short tap on someone else's channel message still
  inserts the reply mention). Back returns to the conversation.
- Received messages show: sender, received time, the sender's own timestamp, the **clock difference**
  (flagged when over 10 minutes), route (flood with hop count, heard directly, or routed), SNR and RSSI
  of the last hop, **times heard**, payload size with estimated airtime, the **repeater path** with each
  hop named when exactly one known repeater matches (otherwise the hash and how many match), and a
  **Show on map** button when the sender's position is known.
- Sent messages show recipient, send time, status, round trip and length.
- Extra data is stored in a new SD record type `D`, written after each received message, so history
  from before this change simply lacks the detail rows.
- "Times heard" is live: duplicate copies are counted while a packet is still in MeshCore's recent
  table (about 160 packets); it reads "once" for older messages.
- Not done: the per-copy paths of duplicates, and a route drawn on the map.

## Decisions
- No further metrics wanted beyond those listed.
