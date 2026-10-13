#pragma once
// Range-test results: the outcome of each acknowledged test message.
#include <stdint.h>

struct SigResult {
  uint32_t ts;
  char name[32];
  bool ok;
  uint32_t rttMs;
  int8_t hops;          // -1 = flood / unknown
  int16_t rssi;         // dBm of the returning ACK
  int8_t snr4;          // SNR in quarter dB
};

void sig_begin(const char* name, uint32_t ts);    // a test has been sent
bool sig_pending();
void sig_finish(const SigResult& r);              // an ACK arrived, or the test timed out
const SigResult* sig_last();                      // most recent finished test, or nullptr
int sig_total();                                  // tests finished since boot
uint32_t sig_version();
