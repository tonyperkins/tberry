#pragma once
// MeshCore's ESP32Board.cpp expects a target.h that declares the global radio driver,
// as each of its board variants provides. Defined in src/main.cpp.
#include <helpers/radiolib/CustomSX1262Wrapper.h>

extern CustomSX1262Wrapper radio_driver;

// ESP32Board::enterDeepSleep() asks `sensors` for a GPS provider to stop. This firmware has no
// sensor manager yet, so a stub that reports "no location provider" satisfies it.
struct StubLocationProvider { void stop() {} };
struct StubSensors { StubLocationProvider* getLocationProvider() { return nullptr; } };
extern StubSensors sensors;
