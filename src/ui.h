#pragma once
// The on-device interface. It knows nothing about MeshCore: main.cpp fills a UiModel with the
// facts to show and forwards input events. Everything here must return quickly.
#include <stdint.h>

struct UiModel {
  uint32_t utc = 0;          // unix seconds, UTC
  bool clockValid = false;
  int batteryPct = 0;
  bool charging = false;
  int signalBars = 0;        // 0..4, how recently the mesh was heard
  int nodesHeard = 0;
  int unread = 0;            // unread messages across all conversations
  char nodeName[32] = {0};   // this node's name on the mesh
  char storage[32] = {0};    // where history is kept, for the Settings screen
  // health, for the Status screen
  int batteryMv = 0;
  uint32_t uptimeSec = 0;
  uint32_t packetsRx = 0, packetsTx = 0, rxErrors = 0;
  uint32_t airtimeMs = 0;    // total time this radio has spent transmitting
  uint32_t loopWorstMs = 0;  // worst main-loop pass in the last minute
  uint32_t heapKb = 0, psramKb = 0;
  uint8_t timeSource = 0;    // 0 none, 1 borrowed from the mesh, 2 GPS
  bool sdReady = false;
  uint32_t sdFreeMb = 0;
  bool hasPos = false;       // a current GPS position
  uint8_t gpsState = 0;      // 0 no data from the module, 1 searching, 2 position fix, 3 switched off
  uint16_t gpsWindowSec = 0; // while GPS is off but in its start-up time window: seconds left
  uint8_t gpsFixType = 0;    // 1 none, 2 = 2D, 3 = 3D
  uint8_t gpsSatsUsed = 0, gpsSatsView = 0;
  float gpsHdop = 0, gpsAltM = 0, gpsSpeedKmh = 0, gpsCourse = 0;
  uint32_t gpsFixAgeSec = 0; // seconds since the last valid position sentence
  int32_t latE6 = 0, lonE6 = 0;
  char consoleText[720] = {0};   // text shown by the debug console
};

void ui_init();                       // after the display, I2C and Serial are up
void ui_tick();                       // call every loop pass
void ui_update(const UiModel& m);     // call a few times per second
void ui_nav(int dx, int dy);          // trackball step
void ui_click();                      // trackball click
void ui_long();                       // trackball long press: go Home
void ui_key(char c);                  // keyboard character
bool ui_console_active();             // true while the debug console owns the keyboard
void ui_activity();                   // any input counts as activity (keeps the screen awake)

// Called when the user sends from a conversation. main.cpp records the outgoing message in the
// store, transmits it, and updates its status. Returns false if it could not be queued.
typedef bool (*UiSendFn)(int conv, const char* text);
void ui_set_send_handler(UiSendFn fn);

// Things Settings can ask the rest of the firmware to do.
struct UiActions {
  void (*applyRadio)();                    // use the radio parameters now in cfg()
  void (*setName)(const char* name);       // rename the node and re-advertise
  void (*advertise)();                     // send an advert now
  bool (*sendTest)(int conv);              // send a range-test message into a conversation
};
void ui_set_actions(const UiActions& a);
