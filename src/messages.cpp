#include "messages.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

// The UI fonts cover Latin-1, Latin Extended-A and common punctuation (see fonts/fonts.h), and emoji
// are drawn from images on the SD card (emoji.cpp). Anything else would show as an empty box:
// modifiers and joiners are dropped, other characters become '?', control characters become
// spaces. Emoji are kept here and filtered at draw time, depending on what the card holds.
static void cleanText(char* dst, size_t n, const char* src) {
  size_t o = 0;
  for (size_t i = 0; src[i] && o + 1 < n;) {
    uint8_t c = (uint8_t)src[i];
    if (c < 0x80) {
      dst[o++] = (c < 0x20) ? ' ' : (char)c;
      i++;
      continue;
    }
    int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    uint32_t cp = len == 4 ? (c & 0x07) : len == 3 ? (c & 0x0F) : len == 2 ? (c & 0x1F) : 0xFFFD;
    int k = 1;
    for (; k < len && src[i + k]; k++) cp = (cp << 6) | ((uint8_t)src[i + k] & 0x3F);
    i += k;
    // Modifiers and joiners are dropped: skin tones, variation selectors, zero-width joiner, tags.
    bool modifier = cp == 0xFE0F || cp == 0xFE0E || cp == 0x200D || (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0000 && cp <= 0xE007F);
    bool pua = cp >= 0xE000 && cp <= 0xF8FF;
    // Emoji are kept; the UI draws them from the SD card when the pack is there and drops them
    // otherwise (emoji_filter), so a card inserted later still shows old messages' emoji.
    bool emoji = cp >= 0x1F000 || (cp >= 0x2190 && cp <= 0x2BFF);
    bool drawable = emoji || (cp >= 0xA0 && cp <= 0x17F) || (cp >= 0x2010 && cp <= 0x2027) || cp == 0x20AC || cp == 0x2122;
    if (modifier || pua) continue;
    if (drawable && o + len < n) {                 // keep the original UTF-8 bytes
      for (int q = 0; q < k; q++) dst[o++] = src[i - k + q];
    } else {
      dst[o++] = '?';
    }
  }
  dst[o] = 0;
}

static Conv convs[MAX_CONVS];
void msgs_clean(char* dst, size_t n, const char* src) { cleanText(dst, n, src); }
static uint32_t version = 1, nextId = 1;
static int openConv = -1;
static const MsgSink* sink = nullptr;     // survives msgs_init(): registered once at boot

void msgs_init() {
  memset(convs, 0, sizeof(convs));
  version = 1;
  nextId = 1;
  openConv = -1;
}

int msgs_find(const uint8_t key[4]) {
  for (int i = 0; i < MAX_CONVS; i++)
    if (convs[i].used && memcmp(convs[i].key, key, 4) == 0) return i;
  return -1;
}

int msgs_ensure(const uint8_t key[4], const char* name, bool channel) {
  int i = msgs_find(key);
  if (i < 0) {
    for (int k = 0; k < MAX_CONVS; k++) if (!convs[k].used) { i = k; break; }
    if (i < 0) return -1;
    memset(&convs[i], 0, sizeof(Conv));
    convs[i].used = true;
    memcpy(convs[i].key, key, 4);
    convs[i].channel = channel;
  }
  char clean[32];
  if (name) cleanText(clean, sizeof(clean), name);
  if (name && strncmp(convs[i].name, clean, sizeof(convs[i].name)) != 0) {
    strncpy(convs[i].name, clean, sizeof(convs[i].name) - 1);
    version++;
  } else if (!name) {
    version++;
  }
  return i;
}

const Conv* msgs_conv(int idx) { return (idx >= 0 && idx < MAX_CONVS && convs[idx].used) ? &convs[idx] : nullptr; }

const Msg* msgs_at(int conv, int i) {
  if (conv < 0 || conv >= MAX_CONVS || !convs[conv].used || i < 0 || i >= convs[conv].count) return nullptr;
  return &convs[conv].msgs[(convs[conv].start + i) % MSGS_PER_CONV];
}

// Shared by new and loaded messages. id == 0 means "assign the next id".
static uint32_t addInternal(int conv, uint32_t id, bool out, const char* text, uint32_t ts, uint8_t status, bool loaded,
                            const MsgInfo* info) {
  if (conv < 0 || conv >= MAX_CONVS || !convs[conv].used) return 0;
  Conv& c = convs[conv];
  if (!c.msgs) {
    c.msgs = (Msg*)heap_caps_calloc(MSGS_PER_CONV, sizeof(Msg), MALLOC_CAP_SPIRAM);
    if (!c.msgs) c.msgs = (Msg*)calloc(MSGS_PER_CONV, sizeof(Msg));
    if (!c.msgs) return 0;
  }
  int slot;
  if (c.count < MSGS_PER_CONV) {
    slot = (c.start + c.count) % MSGS_PER_CONV;
    c.count++;
  } else {
    slot = c.start;                           // evict the oldest
    c.start = (c.start + 1) % MSGS_PER_CONV;
  }
  Msg& m = c.msgs[slot];
  if (id == 0) id = nextId++;
  else if (id >= nextId) nextId = id + 1;
  m.id = id;
  m.ts = ts;
  m.status = status;
  m.out = out;
  m.rtt100 = 0;
  m.hops = -1; m.snr4 = 0; m.rssi = 0; m.flags = 0; m.sender[0] = 0;
  if (info) {
    m.hops = info->hops; m.snr4 = info->snr4; m.rssi = info->rssi; m.flags = info->flags;
    strncpy(m.sender, info->sender, sizeof(m.sender) - 1);
    m.sender[sizeof(m.sender) - 1] = 0;
  }
  cleanText(m.text, sizeof(m.text), text);
  if (ts > c.lastTs) c.lastTs = ts;
  if (!loaded) {
    if (!out && conv != openConv) {
      c.unread++;
      if ((m.flags & MF_MENTION) && c.mentionUnread < 250) c.mentionUnread++;
    }
    if (sink && sink->onAdd) sink->onAdd(c.key, m);
  }
  version++;
  return m.id;
}

uint32_t msgs_add(int conv, bool out, const char* text, uint32_t ts, uint8_t status) {
  return addInternal(conv, 0, out, text, ts, status, false, nullptr);
}
uint32_t msgs_add_info(int conv, bool out, const char* text, uint32_t ts, uint8_t status, const MsgInfo& info) {
  return addInternal(conv, 0, out, text, ts, status, false, &info);
}
uint32_t msgs_add_loaded(int conv, uint32_t id, bool out, const char* text, uint32_t ts, uint8_t status) {
  return addInternal(conv, id, out, text, ts, status, true, nullptr);
}
uint32_t msgs_add_loaded_info(int conv, uint32_t id, bool out, const char* text, uint32_t ts, uint8_t status, const MsgInfo& info) {
  return addInternal(conv, id, out, text, ts, status, true, &info);
}

static void setStatusInternal(int conv, uint32_t id, uint8_t status, uint16_t rtt100, bool loaded) {
  if (conv < 0 || conv >= MAX_CONVS || !convs[conv].used) return;
  Conv& c = convs[conv];
  for (int i = 0; i < c.count; i++) {
    Msg& m = c.msgs[(c.start + i) % MSGS_PER_CONV];
    if (m.id == id) {
      m.status = status;
      if (rtt100) m.rtt100 = rtt100;
      version++;
      if (!loaded && sink && sink->onStatus) sink->onStatus(c.key, id, status, rtt100);
      return;
    }
  }
}
void msgs_set_status(int conv, uint32_t id, uint8_t status, uint16_t rtt100) { setStatusInternal(conv, id, status, rtt100, false); }
void msgs_set_status_loaded(int conv, uint32_t id, uint8_t status, uint16_t rtt100) { setStatusInternal(conv, id, status, rtt100, true); }
void msgs_set_sink(const MsgSink* s) { sink = s; }

void msgs_mark_read(int conv) {
  if (conv >= 0 && conv < MAX_CONVS && convs[conv].used && (convs[conv].unread || convs[conv].mentionUnread)) {
    convs[conv].unread = 0;
    convs[conv].mentionUnread = 0;
    version++;
  }
}

void msgs_set_open(int conv) { openConv = conv; }

int msgs_unread_total() {
  int n = 0;
  for (int i = 0; i < MAX_CONVS; i++) if (convs[i].used) n += convs[i].unread;
  return n;
}

int msgs_sorted(int* out, int max) {
  int n = 0;
  for (int i = 0; i < MAX_CONVS && n < max; i++) if (convs[i].used) out[n++] = i;
  // insertion sort: channels first, then most recent activity, then name
  for (int a = 1; a < n; a++) {
    int v = out[a], b = a - 1;
    auto before = [&](int x, int y) {
      if (convs[x].channel != convs[y].channel) return convs[x].channel;
      if (convs[x].lastTs != convs[y].lastTs) return convs[x].lastTs > convs[y].lastTs;
      return strcmp(convs[x].name, convs[y].name) < 0;
    };
    while (b >= 0 && before(v, out[b])) { out[b + 1] = out[b]; b--; }
    out[b + 1] = v;
  }
  return n;
}

uint32_t msgs_version() { return version; }
