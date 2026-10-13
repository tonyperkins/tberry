#pragma once
// In-memory message store. The mesh callbacks write into it, the UI reads from it.
// Conversations are keyed by the first 4 bytes of a contact's public key; the Public channel
// uses the reserved key below. Message text lives in PSRAM.
#include <stdint.h>

enum MsgStatus : uint8_t { MS_SENDING = 0, MS_SENT = 1, MS_DELIVERED = 2, MS_FAILED = 3, MS_RECEIVED = 4 };

struct Msg {
  uint32_t id;       // unique and increasing; stays valid when old messages are evicted
  uint32_t ts;       // unix seconds, our clock when it was stored
  uint8_t status;    // MsgStatus
  bool out;
  uint16_t rtt100;   // delivery round trip in 100 ms units; 0 = unknown
  char text[164];
};

static constexpr int MAX_CONVS = 24;
static constexpr int MSGS_PER_CONV = 30;
static constexpr uint8_t CHANNEL_KEY[4] = { 0xFF, 0xFF, 0xFF, 0xFF };   // the Public channel

struct Conv {
  bool used;
  bool channel;
  uint8_t key[4];
  char name[32];
  int count;         // messages held, oldest first
  int start;         // ring buffer start
  int unread;
  uint32_t lastTs;
  Msg* msgs;         // MSGS_PER_CONV entries, allocated on first use
};

void msgs_init();
int msgs_find(const uint8_t key[4]);                                  // conversation index or -1
int msgs_ensure(const uint8_t key[4], const char* name, bool channel);
const Conv* msgs_conv(int idx);
const Msg* msgs_at(int conv, int i);                                  // i = 0 is the oldest
uint32_t msgs_add(int conv, bool out, const char* text, uint32_t ts, uint8_t status);   // returns the id
void msgs_set_status(int conv, uint32_t id, uint8_t status, uint16_t rtt100 = 0);
void msgs_mark_read(int conv);
void msgs_set_open(int conv);                                         // the conversation on screen, or -1
int msgs_unread_total();
int msgs_sorted(int* out, int max);                                   // channel first, then newest activity
uint32_t msgs_version();

// Storage hooks. The store itself knows nothing about SD cards: whoever persists messages
// registers a sink and is told about every new message and every status change.
struct MsgSink {
  void (*onAdd)(const uint8_t key[4], const Msg& m);
  void (*onStatus)(const uint8_t key[4], uint32_t id, uint8_t status, uint16_t rtt100);
};
void msgs_set_sink(const MsgSink* sink);
// Insert a message read back from storage: keeps its id, is never unread, and is not re-saved.
uint32_t msgs_add_loaded(int conv, uint32_t id, bool out, const char* text, uint32_t ts, uint8_t status);
void msgs_set_status_loaded(int conv, uint32_t id, uint8_t status, uint16_t rtt100 = 0);   // same, for a status record
                                              // changes whenever anything does
