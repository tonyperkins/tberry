# TDOS-008: Recognise reactions

**Type:** feature  **Status:** phases 0 and 1 done; phase 2 waiting on research  **Area:** Messages (thread view), research  **Depends on:** TDOS-007 helpful

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

## Phase 1 as built
- A reaction is attached to the **latest earlier message from the author it names** (the same lookup
  TDOS-007 uses for quotes), and shown as a small amber badge line under that message:
  `🎉 WRMattil   👍 Greg`. The standalone reaction line is hidden when its target is on screen, and kept
  when it is not.
- This is a heuristic. If the author posted several messages since, the badge can land on the wrong
  one. Resolving the 8 character code would make it exact.

## Research so far
- Four real reactions were tested against MD5, SHA-1/224/256/384/512, SHA3, BLAKE2, FNV, CRC32 and djb2
  of the target's text in several forms, taking every plausible slice. **No match.**
- The four codes decode to 36, 39, 39 and 41 bits as base 36, so they are uniform over 36^8, not 40-bit
  or 32-bit values. That rules out five bytes of a packet hash and CRC32.
- The sample did not include the sender timestamps or packet hashes a code might be derived from, so the
  search needs real logs.

## Data collection (running)
The firmware writes `/tdos/idlog.csv` on the SD card: for every channel message the receive time, the
sender's timestamp, hop count, the full 8 byte packet hash and the raw text. When a few reactions have
been logged, run `python3 tools/find_reaction_id.py idlog.csv`. It tries the derivations against every
reaction found in the file and prints any that reproduce the code.

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

## Phase 0 as built
- `src/reaction.h` holds `parseReaction()`, a strict recogniser. It is applied when the thread is
  drawn, so reactions already in the history are tidied too.
- A reaction renders as one quiet line, `Sender reacted 🎉 to Author` (accent colour when it is to
  this node), with the code hidden. The conversation preview reads `Sender reacted 🎉`.
- A reaction does not chime and is not treated as a mention.
- `tests/reaction_test.cpp` covers the sample lines and the near-misses that must not match.

## Acceptance criteria (phase 0)
- Reaction lines never show as raw `@[Name]😂 xxxxxxxx` text.
- The emoji, reactor and author are readable on one short line.
- A genuine message that happens to look similar is not hidden: require the code to be exactly 8
  characters of `0-9a-z` and the emoji to be a single emoji code point (plus optional modifiers).

## Open questions
- Should reactions to **our** messages chime? Probably not, but a small badge on the conversation row
  is reasonable.
