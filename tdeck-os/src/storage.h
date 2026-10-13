#pragma once
// SD card storage. Message history lives here; identity, settings and contacts stay in the
// device's own flash so the node works, and keeps its identity, without a card.
#include <SPI.h>
#include <stdint.h>

bool storage_begin(SPIClass& spi, int csPin);   // mounts the card and registers the message sink
bool storage_ready();                           // a card is mounted and usable
uint32_t storage_free_mb();
void storage_load_messages();                   // after the conversations exist
void storage_remove_conv(const uint8_t key[4]);   // delete a conversation's history file
void storage_log_boot(const char* line);   // one line per start to /tdos/boot.log
void storage_log_signal(uint32_t ts, const char* name, bool ok, uint32_t rttMs, int hops, int rssi, float snr);
// Research for TDOS-008: one CSV line per channel message with the fields a reaction code might be
// derived from (their timestamp, the full packet hash, the raw text), so the code can be reverse
// engineered offline once a few reactions have been seen. /tdos/idlog.csv, capped at 3 MB.
void storage_log_idrow(uint32_t rxTs, uint32_t senderTs, int hops, const uint8_t hash8[8], const char* rawText);
void storage_tick();                            // call every loop pass; writes are batched
