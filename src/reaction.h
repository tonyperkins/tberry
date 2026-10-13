#pragma once
// MeshCore reactions travel as ordinary channel text:
//
//     @[<author of the message being reacted to>]<emoji> <8 characters of 0-9a-z>
//
// e.g.  @[WRMattil]🎉 59pv3qap
//
// Clients that do not understand them show that raw line. parseReaction() recognises the pattern
// strictly (no space before the emoji, exactly eight code characters, nothing after) so that a
// normal reply that merely looks similar is never hidden. See docs/backlog/TDOS-008-reactions.md.
#include <stdint.h>
#include <string.h>

static inline bool reactionEmojiCp(uint32_t cp) {
  return (cp >= 0x1F000 && cp <= 0x1FFFF) || (cp >= 0x2190 && cp <= 0x2BFF) || cp == 0xFE0F || cp == 0x200D;
}

static inline bool parseReaction(const char* s, char* author, size_t authorN, char* emoji, size_t emojiN,
                                 char* code, size_t codeN) {
  if (!s || s[0] != '@' || s[1] != '[') return false;
  const char* close = strchr(s + 2, ']');
  if (!close) return false;
  size_t alen = (size_t)(close - (s + 2));
  if (alen == 0 || alen >= authorN) return false;

  const char* p = close + 1;
  const char* e0 = p;
  int cps = 0;
  while (*p && (uint8_t)*p >= 0x80) {
    uint8_t c = (uint8_t)*p;
    int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
    if (len == 0) return false;                     // a stray continuation byte
    uint32_t cp = len == 4 ? (c & 0x07) : len == 3 ? (c & 0x0F) : (c & 0x1F);
    for (int k = 1; k < len; k++) {
      if (!p[k]) return false;
      cp = (cp << 6) | ((uint8_t)p[k] & 0x3F);
    }
    if (!reactionEmojiCp(cp)) return false;
    if (cps == 0 && (cp == 0xFE0F || cp == 0x200D)) return false;   // must open with a real emoji
    p += len;
    if (++cps > 6) return false;                    // a reaction is one emoji, maybe with modifiers
  }
  if (cps == 0) return false;
  size_t elen = (size_t)(p - e0);
  if (elen >= emojiN) return false;
  if (*p != ' ') return false;
  p++;

  size_t i = 0;
  while (p[i] && ((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'z'))) i++;
  if (i != 8 || codeN < 9) return false;
  const char* tail = p + 8;
  while (*tail == ' ' || *tail == '\n' || *tail == '\r') tail++;
  if (*tail) return false;

  memcpy(author, s + 2, alen); author[alen] = 0;
  memcpy(emoji, e0, elen); emoji[elen] = 0;
  memcpy(code, p, 8); code[8] = 0;
  return true;
}
