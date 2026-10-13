# TDOS-006: Highlight and chime for messages that mention me

**Type:** feature  **Status:** done  **Area:** Messages, audio  **Depends on:** TDOS-003 (shares the name parsing)

## Why
The public channel is silent by design (too busy to chime). But when someone replies to you, with
`@[YourName]`, you want to know. In the sample that is exactly how conversations in the channel are
threaded.

## Behaviour
1. A channel message containing `@[<our node name>]` is a **mention**.
2. It plays the incoming-message chime, even in a channel.
3. Its bubble is drawn with an amber edge, and the conversation list shows a mention marker next to
   the unread count.
4. Messages that do not mention us stay silent, as now.

## Acceptance criteria
- A reply `@[TDeck-9B59] hello` to a node named TDeck-9B59 chimes and is highlighted.
- A message mentioning someone else does not.
- Renaming the node updates what counts as a mention immediately.
- The match is on the exact name, including emoji and spacing, and is case sensitive.

## Technical notes
- Compare against the **raw** received text, not the sanitised copy. Our sanitiser drops joiners and
  variation selectors that other users' names may contain, so a sanitised comparison would miss them.
- Add a `mention` flag to `Msg` and to the log record (same migration approach as TDOS-001).
- Add a Settings switch "Channel sounds: off / mentions only / all", default "mentions only".
