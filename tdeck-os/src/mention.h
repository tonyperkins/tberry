#pragma once
// Replies in a MeshCore channel start with a mention: "@[Name] text". These helpers pull the name
// out. See docs/backlog/TDOS-003 and TDOS-007.
#include <stddef.h>
#include <string.h>

// True if `text` begins with "@[Name]" (optionally after leading spaces). `name` receives Name.
static inline bool leadingMention(const char* text, char* name, size_t nameN) {
  if (!text) return false;
  while (*text == ' ') text++;
  if (text[0] != '@' || text[1] != '[') return false;
  const char* close = strchr(text + 2, ']');
  if (!close) return false;
  size_t len = (size_t)(close - (text + 2));
  if (len == 0 || len >= nameN) return false;
  memcpy(name, text + 2, len);
  name[len] = 0;
  return true;
}
