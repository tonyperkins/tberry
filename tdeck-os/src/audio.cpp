#include "audio.h"
#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>

static constexpr i2s_port_t PORT = I2S_NUM_0;
static constexpr int PIN_BCLK = 7, PIN_WS = 5, PIN_DOUT = 6;
static constexpr int RATE = 16000;

static QueueHandle_t queue = nullptr;
static volatile uint8_t volume = 60;

struct Tone { uint16_t hz; uint16_t ms; };           // hz 0 = silence; {0,0} ends a sequence

static const Tone SEQ_BOOT[]      = { {523, 70}, {659, 70}, {784, 120}, {0, 0} };
static const Tone SEQ_RECEIVE[]   = { {880, 70}, {1175, 120}, {0, 0} };
static const Tone SEQ_SENT[]      = { {660, 55}, {0, 0} };
static const Tone SEQ_DELIVERED[] = { {1320, 35}, {0, 20}, {1760, 45}, {0, 0} };
static const Tone SEQ_FAILED[]    = { {330, 130}, {0, 40}, {262, 200}, {0, 0} };
static const Tone SEQ_TEST_OK[]   = { {784, 60}, {988, 60}, {1175, 140}, {0, 0} };
static const Tone SEQ_GPS[]       = { {988, 70}, {0, 40}, {1319, 130}, {0, 0} };
static const Tone SEQ_LOWBATT[]   = { {440, 120}, {0, 80}, {440, 120}, {0, 80}, {440, 120}, {0, 0} };
static const Tone SEQ_BLIP[]      = { {880, 45}, {0, 0} };

static const Tone* sequenceFor(uint8_t s) {
  switch (s) {
    case SND_BOOT: return SEQ_BOOT;
    case SND_RECEIVE: return SEQ_RECEIVE;
    case SND_SENT: return SEQ_SENT;
    case SND_DELIVERED: return SEQ_DELIVERED;
    case SND_FAILED: return SEQ_FAILED;
    case SND_TEST_OK: return SEQ_TEST_OK;
    case SND_GPS_LOCK: return SEQ_GPS;
    case SND_LOW_BATTERY: return SEQ_LOWBATT;
    default: return SEQ_BLIP;
  }
}

static void writeTone(const Tone& t, float amp) {
  int16_t buf[2 * 128];                                  // stereo frames, the same sample on both
  uint32_t total = (uint32_t)RATE * t.ms / 1000;
  const uint32_t ramp = RATE / 250;                      // 4 ms fade in and out avoids clicks
  float inc = 2.0f * (float)M_PI * t.hz / RATE, phase = 0;
  uint32_t done = 0;
  while (done < total) {
    int n = (int)min<uint32_t>(128, total - done);
    for (int i = 0; i < n; i++) {
      uint32_t pos = done + i;
      float env = 1.0f;
      if (pos < ramp) env = (float)pos / ramp;
      else if (total - pos < ramp) env = (float)(total - pos) / ramp;
      float v = t.hz ? sinf(phase) * env * amp : 0.0f;
      phase += inc;
      int16_t s = (int16_t)(v * 32000.0f);
      buf[2 * i] = s;
      buf[2 * i + 1] = s;
    }
    size_t written;
    i2s_write(PORT, buf, n * 4, &written, portMAX_DELAY);
    done += n;
  }
}

static void audioTask(void*) {
  for (;;) {
    uint8_t s;
    if (xQueueReceive(queue, &s, portMAX_DELAY) != pdTRUE) continue;
    uint8_t v = volume;
    if (v == 0) continue;
    float level = v / 100.0f;
    float amp = 0.7f * level * level;                    // perceived loudness is roughly quadratic
    for (const Tone* t = sequenceFor(s); t->ms; t++) writeTone(*t, amp);
    // a little silence so the amplifier settles and the next sound starts clean
    Tone gap = { 0, 20 };
    writeTone(gap, 0);
  }
}

void audio_begin() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = 128;
  cfg.tx_desc_auto_clear = true;                          // silence, not a repeating buffer, when idle
  if (i2s_driver_install(PORT, &cfg, 0, nullptr) != ESP_OK) return;
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = PIN_BCLK;
  pins.ws_io_num = PIN_WS;
  pins.data_out_num = PIN_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  i2s_set_pin(PORT, &pins);

  queue = xQueueCreate(8, sizeof(uint8_t));
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 1, nullptr, 0);   // core 0; the UI runs on core 1
}

void audio_play(Sound s) {
  if (!queue || volume == 0) return;
  uint8_t v = (uint8_t)s;
  xQueueSend(queue, &v, 0);                               // never wait: if it is full, drop the sound
}

void audio_set_volume(uint8_t percent) { volume = percent > 100 ? 100 : percent; }
