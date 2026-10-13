# TDOS-008: Recognise reactions

**Type:** feature  **Status:** open  **Area:** Messages (thread view), research  **Depends on:** TDOS-007 helpful

## Why
Reactions travel as ordinary channel text, so a client that does not understand them shows lines
like `@[WRMattil]🎉 59pv3qap`. In the sample these are a visible fraction of the traffic, and they are
unreadable noise on the T-Deck today.

## The format, as seen
```
@[<author of the message being reacted to>]<emoji> <8 characters>
```
Examples from the sample:

| Raw text | Reading |
|---|---|
| `@[WRMattil]🎉 59pv3qap` | reacting with 🎉 to a message by WRMattil |
| `@[Wintermute]😂 0nr3ajmf` | 😂 to a message by Wintermute |
| `@[Tommy…]❤️ qqrs66j4` | ❤️ |
| `@[Vasuul]👋 6bhzfn10` | 👋 |

Notes:
- The code is 8 characters of `0-9a-z`, sometimes with leading zeros. It looks like a base-36 number and
  probably identifies the target message. Its value exceeds 32 bits, so it is not a plain CRC32.
- One client renders a reaction in words and keeps the raw suffix:
  `👋 reacted to "@ [🤷‍♂️NBDY] 👋" @[Vasuul] 6bhzfn10`. That client could resolve the code to the
  original message text, so the id is derivable from information receivers already have.

## Phases
**Phase 0, cheap, no research needed.** Detect the pattern. Show a compact line instead of a bubble:
`WRMattil  reacted 🎉  to Wintermute`, hiding the code.

**Phase 1, needs the id derivation.** Resolve the code to the target message and show the emoji as a
small badge on that bubble, with a count when several people reacted.

**Phase 2, sending.** Long-press a message, choose an emoji, send `@[Author]<emoji> <id>`.
Emoji entry needs a picker backed by the SD emoji pack (the keyboard has no emoji).

## Research task: how is the id computed?
Candidates, cheapest to test first:
1. The first 5 bytes of the target message's **packet hash** (`Packet::calculatePacketHash`, 8 bytes
   in total) written in base 36 and zero-padded to 8 characters. Five bytes is 40 bits, which fits an
   8-character base-36 value (up to about 41 bits), and fits all four examples.
2. A hash of the target's sender timestamp plus its text.
3. A hash of the sender name plus the text.

Test: have the device log, for every received channel message, the base-36 forms of candidate 1 and
candidates 2 and 3 to the SD card, beside the text and sender. When a reaction line arrives from the
channel, look which candidate equals its code. A few reactions are enough to decide.

## Acceptance criteria (phase 0)
- Reaction lines never show as raw `@[Name]😂 xxxxxxxx` text.
- The emoji, reactor and author are readable on one short line.
- A genuine message that happens to look similar is not hidden: require the code to be exactly 8
  characters of `0-9a-z` and the emoji to be a single emoji code point (plus optional modifiers).

## Open questions
- Should reactions to **our** messages chime? Probably not, but a small badge on the conversation row
  is reasonable.
