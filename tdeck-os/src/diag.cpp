#include "diag.h"
#include <Arduino.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include "ui.h"

// Survives panic, watchdog and software resets; the magic tells whether it is meaningful.
struct Crumb {
  uint32_t magic;
  uint32_t boots;
  uint32_t upSeconds;
  uint32_t heapFree;
  uint32_t heapMin;
  uint32_t lvglFree;
  uint32_t loopMaxMs;
  int32_t mode;
};
static RTC_NOINIT_ATTR Crumb crumb;
static constexpr uint32_t MAGIC = 0x5444424B;      // "TDBK"
static constexpr uint32_t WDT_SECONDS = 20;        // the loop does no single job this long

static char reason[24] = "Unknown";
static char line[160] = "";
static uint32_t lastTick = 0;

static const char* reasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "Power on";
    case ESP_RST_SW: return "Restart";
    case ESP_RST_PANIC: return "Crash";
    case ESP_RST_INT_WDT: return "Watchdog (interrupt)";
    case ESP_RST_TASK_WDT: return "Watchdog";
    case ESP_RST_WDT: return "Watchdog";
    case ESP_RST_BROWNOUT: return "Brown-out (low power)";
    case ESP_RST_DEEPSLEEP: return "Wake from sleep";
    default: return "Other";
  }
}

void diag_begin() {
  esp_reset_reason_t r = esp_reset_reason();
  snprintf(reason, sizeof(reason), "%s", reasonName(r));
  bool valid = crumb.magic == MAGIC;
  if (valid && r != ESP_RST_POWERON) {
    snprintf(line, sizeof(line), "boot#%lu reason=%s prev: up=%lus screen=%ld heap=%lu min=%lu lvgl=%lu loopmax=%lums",
             (unsigned long)(crumb.boots + 1), reason, (unsigned long)crumb.upSeconds, (long)crumb.mode,
             (unsigned long)crumb.heapFree, (unsigned long)crumb.heapMin, (unsigned long)crumb.lvglFree,
             (unsigned long)crumb.loopMaxMs);
    crumb.boots++;
  } else {
    uint32_t boots = valid ? crumb.boots + 1 : 1;
    memset(&crumb, 0, sizeof(crumb));
    crumb.magic = MAGIC;
    crumb.boots = boots;
    snprintf(line, sizeof(line), "boot#%lu reason=%s", (unsigned long)boots, reason);
  }
}

void diag_watchdog_start() {
  esp_task_wdt_init(WDT_SECONDS, true);            // true: a trip is a panic, so it resets and is recorded
  enableLoopWDT();                                 // the Arduino loop task must come round within the timeout
}

void diag_tick(uint32_t loopMaxMs) {
  uint32_t now = millis();
  if (now - lastTick < 2000) return;
  lastTick = now;
  crumb.magic = MAGIC;
  crumb.upSeconds = now / 1000;
  crumb.heapFree = ESP.getFreeHeap();
  crumb.heapMin = ESP.getMinFreeHeap();
  crumb.lvglFree = ui_lvgl_free();
  crumb.loopMaxMs = loopMaxMs;
  crumb.mode = ui_mode();
}

const char* diag_reason() { return reason; }
const char* diag_line() { return line; }
