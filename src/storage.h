#pragma once
// SD card storage. Message history lives here; identity, settings and contacts stay in the
// device's own flash so the node works, and keeps its identity, without a card.
#include <SPI.h>
#include <stdint.h>

bool storage_begin(SPIClass& spi, int csPin);   // mounts the card and registers the message sink
bool storage_ready();                           // a card is mounted and usable
uint32_t storage_free_mb();
void storage_load_messages();                   // after the conversations exist
void storage_log_signal(uint32_t ts, const char* name, bool ok, uint32_t rttMs, int hops, int rssi, float snr);
void storage_tick();                            // call every loop pass; writes are batched
