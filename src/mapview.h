#pragma once
// Offline map: slippy-map PNG tiles from the SD card (/maps/<z>/<x>/<y>.png), drawn into a canvas
// with markers for this device and the nodes that advertise a position.
#include <lvgl.h>
#include "ui.h"

// Builds the map on `parent` below the status bar. Call mapview_destroy() before the parent goes.
void mapview_create(lv_obj_t* parent);
void mapview_destroy();
void mapview_update(const UiModel& m);   // position and nodes; cheap, call a few times per second
void mapview_tick();                     // finishes progressive tile loading; call every loop pass
void mapview_pan(int dx, int dy);        // by whole screen pixels
void mapview_zoom(int delta);
void mapview_centre_on_me();
bool mapview_active();
// Show a specific place (for example where a message came from): centred, zoomed in, with its own
// highlighted marker and name, and the distance and bearing from this device when we have a fix.
void mapview_focus(double lat, double lon, const char* name);
void mapview_clear_focus();
