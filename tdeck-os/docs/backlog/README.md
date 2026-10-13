# Backlog

One file per ticket. Status values: `open`, `in progress`, `done`. Edit the file when status changes.

| ID | Title | Type | Status |
|---|---|---|---|
| [TDOS-001](TDOS-001-message-radio-info.md) | Show hops, SNR and RSSI on received messages | feature | done |
| [TDOS-002](TDOS-002-message-details.md) | Message details sheet: path, timing and other metrics | feature | done |
| [TDOS-003](TDOS-003-tap-to-reply-mention.md) | Tap a channel message to insert an `@[Name]` reply mention | feature | done |
| [TDOS-004](TDOS-004-show-message-on-map.md) | Show a message's origin on the map | feature | done |
| [TDOS-005](TDOS-005-gps-data-validity.md) | GPS tiles show impossible values without a real fix | bug | done |
| [TDOS-006](TDOS-006-highlight-mentions-of-me.md) | Highlight and chime for messages that mention me | feature | done |
| [TDOS-007](TDOS-007-quote-parent-message.md) | Show the message being replied to above a reply | feature | done |
| [TDOS-008](TDOS-008-reactions.md) | Recognise reactions | feature, research | phase 0 done |
| [TDOS-009](TDOS-009-gps-power-toggle.md) | GPS on/off switch in Settings and the quick panel | feature | done |

Background for all of them: the radio hands every message callback the whole `mesh::Packet`, so hop
count, route type, path, SNR and payload size are available at the moment a message arrives. The
RSSI comes from the radio driver's "last packet" value read in the same callback.
