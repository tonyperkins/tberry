#pragma once
// Crash diagnostics: why did the device restart, and what was it doing.
// - the reset reason is read at boot and shown in Settings > About
// - a few values are kept in RTC memory (they survive a panic or watchdog reset) and written to
//   /tdos/boot.log on the next boot
// - a task watchdog turns a stuck main loop into a reset with a recorded reason
#include <stdint.h>

void diag_begin();                         // very early in setup(): reads the reset reason and the breadcrumbs
void diag_watchdog_start();                // end of setup(): arm the loop watchdog
void diag_tick(uint32_t loopMaxMs);        // main loop: refresh the breadcrumbs (cheap, every 2 s)
const char* diag_reason();                 // "Power on", "Crash", "Watchdog", ...
const char* diag_line();                   // one-line summary of the previous run, for boot.log
