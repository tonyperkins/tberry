# TDOS-003: Tap a channel message to insert an @[Name] reply mention

**Type:** feature  **Status:** open  **Area:** Messages (thread view)

## Why
MeshCore users address someone in a shared channel by writing their node name as `@[Name]` at the
start of the message. Typing the name, especially one with emoji or odd punctuation, is slow on a
small keyboard. Tapping the message you are answering should do it for you.

Example seen on the mesh: `KJ5GFX Bill ... @[WA5AZQ-T1000E] ... @[KJ5GFX Bill ...] morning`.

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

## Open questions
1. Insert at the **start** of the message always, or at the cursor? (Start is the convention I
   assume; please confirm.)
2. Do the apps you use also quote the original text, or only add the mention?
3. Should a message that mentions **our own name** be highlighted and chime even in a channel?
   (Related, probably a separate ticket.)
