#include "messages.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

static Conv convs[MAX_CONVS];
static uint32_t version = 1, nextId = 1;
static int openConv = -1;

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
  if (name && strncmp(convs[i].name, name, sizeof(convs[i].name)) != 0) {
    strncpy(convs[i].name, name, sizeof(convs[i].name) - 1);
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

uint32_t msgs_add(int conv, bool out, const char* text, uint32_t ts, uint8_t status) {
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
  m.id = nextId++;
  m.ts = ts;
  m.status = status;
  m.out = out;
  strncpy(m.text, text, sizeof(m.text) - 1);
  m.text[sizeof(m.text) - 1] = 0;
  c.lastTs = ts ? ts : c.lastTs;
  if (!out && conv != openConv) c.unread++;
  version++;
  return m.id;
}

void msgs_set_status(int conv, uint32_t id, uint8_t status) {
  if (conv < 0 || conv >= MAX_CONVS || !convs[conv].used) return;
  Conv& c = convs[conv];
  for (int i = 0; i < c.count; i++) {
    Msg& m = c.msgs[(c.start + i) % MSGS_PER_CONV];
    if (m.id == id) { m.status = status; version++; return; }
  }
}

void msgs_mark_read(int conv) {
  if (conv >= 0 && conv < MAX_CONVS && convs[conv].used && convs[conv].unread) { convs[conv].unread = 0; version++; }
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
