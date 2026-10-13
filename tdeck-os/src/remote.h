#pragma once
// Debug-only remote control over the USB serial port (build with `pio run -e tdeck-remote`).
// A host script (tools/remote.py) sends one command per line and the device answers "OK", so a
// session can drive the touch screen, keyboard and trackball and fetch screenshots. Normal builds
// compile this to nothing.
#include <lvgl.h>

#ifdef TDOS_REMOTE
void remote_begin();
void remote_poll();                                              // main loop: read commands, stream replies
bool remote_streaming();                                         // a binary frame is going out: no log lines
bool remote_touch(int16_t& x, int16_t& y, bool& have, bool& down);   // injected touch for this poll, if any
void remote_capture(const lv_area_t* a, const lv_color_t* c);    // called with every flushed area
#else
static inline void remote_begin() {}
static inline void remote_poll() {}
static inline bool remote_streaming() { return false; }
static inline bool remote_touch(int16_t&, int16_t&, bool&, bool&) { return false; }
static inline void remote_capture(const lv_area_t*, const lv_color_t*) {}
#endif
