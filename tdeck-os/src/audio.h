#pragma once
// Sound effects through the T-Deck's I2S speaker amplifier. Everything is played by a background
// task, so audio_play() returns immediately and the UI never waits for a sound to finish.
#include <stdint.h>

enum Sound : uint8_t {
  SND_BOOT, SND_RECEIVE, SND_SENT, SND_DELIVERED, SND_FAILED,
  SND_TEST_OK, SND_GPS_LOCK, SND_LOW_BATTERY, SND_BLIP,
};

void audio_begin();
void audio_play(Sound s);
void audio_set_volume(uint8_t percent);    // 0 = muted .. 100
