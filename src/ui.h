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
};
void ui_set_actions(const UiActions& a);
