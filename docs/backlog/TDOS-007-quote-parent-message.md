# TDOS-007: Show the message being replied to above a reply

**Type:** feature  **Status:** open  **Area:** Messages (thread view)  **Depends on:** TDOS-003

## Why
In a busy channel a reply such as `@[Greg] morning from Trenton TX!` is meaningless without what it
answers. Other apps solve this by quoting the parent above the reply. It is the thing that makes a
shared channel readable as conversations.

## What the sample shows
- A reply is an ordinary channel message starting with `@[Name]`. Nothing extra is transmitted.
- The receiving app finds the **latest earlier message from the sender named in the mention** and
  shows it as a quote: sender name, then the text cut to about 95 characters.
- A reply carries a thread marker, and the quote can be followed to its parent.
- When the sender named has said nothing recent enough to find, apps simply show the reply plain.

## Behaviour
1. For a message whose text starts with `@[Name]` (or contains one near the start), look up the most
   recent earlier message in this conversation whose sender is `Name`.
2. If found, draw a compact quote block above the bubble: a thin amber bar on the left, the sender in
   muted text, and one line of the parent (ellipsis at the end).
3. Tapping the quote scrolls to the parent and flashes it.
4. If not found, show only a small "reply to Name" line, no quote.
5. A small thread icon marks any message that is a reply or has replies.

## Acceptance criteria
- A reply to a message still in memory shows the right parent.
- Names with emoji and spaces match (compare the raw sender names, as in TDOS-003).
- A message with a mention of someone with no known earlier message still renders normally.
- The quote never makes a bubble wider than the bubble limit, and wraps or truncates cleanly.

## Technical notes
- The store keeps only 30 messages per conversation, so on a busy channel the parent is often gone.
  Increase the in-memory window (it is PSRAM, so 200 per channel costs about 35 KB per
  conversation), and/or load older messages from the SD log on demand.
- Needs the sender name stored **per channel message** (raw and sanitised): the field TDOS-003 adds.
- Matching "latest earlier message from this sender" can pick the wrong one when someone posted
  several lines. That is how the other apps behave too, and is accepted.
- Consider a "thread view" later (tap the thread icon to see the parent and every reply), but it is not
  needed for this ticket.

## Open questions
- Hide the leading `@[Name]` in the reply bubble when a quote is shown, or keep it as sent? Other apps
  keep it. Keeping it is the simpler and more honest choice.
