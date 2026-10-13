# TDOS-003: Tap a channel message to insert an @[Name] reply mention

**Type:** feature  **Status:** done  **Area:** Messages (thread view)

## Why
MeshCore users address someone in a shared channel by writing their node name as `@[Name]` at the
start of the message. Typing the name, especially one with emoji or odd punctuation, is slow on a
small keyboard. Tapping the message you are answering should do it for you.

Example seen on the mesh: `KJ5GFX Bill ... @[WA5AZQ-T1000E] ... @[KJ5GFX Bill ...] morning`.

## What real traffic shows
From about 250 messages of the Austin-area public channel (pasted by the owner, 2026-10-13):

| Pattern | Example | How common |
|---|---|---|
| `@[Name] text` | `@[Wy-Bo] morning from Maryville TN` | by far the most common; the de facto standard |
| `@[Name]: text` | `@[IneffableAwe]: Mobile, al here that close ?` | one client, used consistently by two or three people |
| `@[Name]<emoji> <id>` | `@[WRMattil]🎉 59pv3qap` | a **reaction**, not a reply (see below) |
| bare `@ Name text` or no mention | `@ AJ4TW Where are you located.` | rare; people typing by hand |

Observations:
- The name is the sender's **exact display name**, including spaces, parentheses, emoji and ZWJ
  sequences: `@[FEAR_RADIO_CAR (Aries)🐺]`, `@[N5LRQ ( Jimmy)]`, `@[🤷‍♂️NBDY]`, `@[J-Horn🇺🇸]`.
- Receiving clients show a **quote of the parent message above the reply** (marked with a thread
  icon). That is purely client-side: they find the sender named in the mention and quote that
  sender's latest message, truncated to about 95 characters. Nothing extra goes on the air.
- So the convention a reply needs to follow is just `@[Name] ` at the start. That is close enough to
  a standard for everything seen here.
- **Reactions** are sent as text: `@[Sender]` immediately followed by an emoji, a space, and an
  8-character code (`59pv3qap`, `0nr3ajmf`, `6bhzfn10`). The code presumably identifies the reacted-to
  message. Apps without reaction support show the raw text, so those lines look like noise.

## Behaviour
1. In a **channel** conversation, tapping an **incoming** bubble inserts `@[Sender] ` (with a trailing
   space) at the start of the message box and puts the cursor after it.
2. If the box already starts with a mention of that same sender, do nothing (no double mention).
3. If the box starts with a different mention, replace it, so one tap per person switches the target.
4. Tapping your own bubbles, or any bubble in a direct conversation, does nothing.
5. Brief visual feedback: the tapped bubble flashes.

## Acceptance criteria
- After tapping a message from "Sam", the box reads `@[Sam] ` and typing continues after it.
- The sender's name is inserted **exactly** as they transmitted it, including emoji and spaces.
- The 140-character message limit still counts the mention.
- A name containing `]` or longer than the limit does not corrupt the box.

## Technical notes
- Channel messages arrive as `Name: text`. The sender is everything before the first `": "`.
- **Our sanitiser alters text on the way into the store** (drops ZWJ and variation selectors, turns
  unsupported characters into `?`). The mention must use the original bytes, so store the raw sender
  name separately on the message (new field, up to 32 bytes) before sanitising.
- The thread currently has no per-bubble tap handler, and touch scrolling shares the surface. Use a
  click event (LVGL does not fire it after a drag) and leave long-press for TDOS-002.

## Decisions (from the sample)
- Insert at the **start** of the box, format `@[Name] ` (one space), not the colon variant.
- Quoting the parent is a **display** feature, not part of the reply. Possible follow-up ticket;
  see TDOS-007 below if raised.
- Reactions are a separate feature (not in scope here); until supported, show them as plain text.

## Open questions
- None blocking. Highlighting messages that mention our own name is TDOS-006.
