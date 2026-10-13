// tdeck-hello: bring-up firmware for the LilyGO T-Deck Plus.
//
// Shows live status of every peripheral on the screen:
//   display, touch (GT911), keyboard (I2C 0x55), trackball, SD card, LoRa (SX1262), battery.
// Design rules carried over from what went wrong in other firmware:
//   * the trackball click is interrupt-driven and latched, never polled
//   * serial logging must never block the main loop
//   * no screensaver or CPU frequency switching
//
// LoRa: US preset 910.525 MHz / BW 62.5 / SF7 / CR5, MeshCore sync word and preamble,
// so it receives raw MeshCore traffic. Press 't' to send a test frame (low power).

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <RadioLib.h>

// ── Pins (T-Deck Plus) ───────────────────────────────────────────
static constexpr int PIN_POWERON = 10;
static constexpr int PIN_LORA_CS = 9, PIN_LORA_RST = 17, PIN_LORA_DIO1 = 45, PIN_LORA_BUSY = 13;
static constexpr int PIN_SD_CS = 39, PIN_TFT_CS = 12;
static constexpr int PIN_I2C_SDA = 18, PIN_I2C_SCL = 8;
static constexpr int PIN_TB_UP = 3, PIN_TB_DOWN = 15, PIN_TB_LEFT = 1, PIN_TB_RIGHT = 2, PIN_TB_CLICK = 0;
static constexpr int PIN_BATT = 4;
static constexpr uint8_t KB_ADDR = 0x55;

// ── Radio parameters ─────────────────────────────────────────────
static constexpr float   LORA_FREQ = 910.525f;
static constexpr float   LORA_BW   = 62.5f;
static constexpr uint8_t LORA_SF   = 7;
static constexpr uint8_t LORA_CR   = 5;
static constexpr int8_t  LORA_TX_DBM = 2;     // test frames only; deliberately low

TFT_eSPI tft;
// The radio and SD card must share the display driver's SPI object. RadioLib's default (the
// global SPI) re-attaches the chip's default pins and silently breaks the display.
static SX1262* radio = nullptr;

// ── Interrupt-driven trackball ───────────────────────────────────
static volatile uint32_t isrUp, isrDown, isrLeft, isrRight, isrClick, isrClickRaw;
static void IRAM_ATTR onUp()    { isrUp++; }
static void IRAM_ATTR onDown()  { isrDown++; }
static void IRAM_ATTR onLeft()  { isrLeft++; }
static void IRAM_ATTR onRight() { isrRight++; }
static void IRAM_ATTR onClick() {
  static uint32_t last;
  isrClickRaw++;                      // every edge, before debounce: shows an interrupt storm
  uint32_t now = micros();
  if (digitalRead(PIN_TB_CLICK) == LOW && now - last > 20000) { isrClick++; last = now; }
}

// ── Radio RX flag ────────────────────────────────────────────────
static volatile bool rxFlag = false;
static void IRAM_ATTR onRadio() { rxFlag = true; }

// ── Logging that can never block ─────────────────────────────────
// HWCDC::write() waits for the USB host to drain the buffer. With nobody reading the port
// that wait is unbounded (setTxTimeoutMs(0) makes it wrap and wait ~49 days). So only write
// when the whole line fits in the free buffer; otherwise drop it.
static void logf(const char* fmt, ...) {
  char b[200];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n > (int)sizeof(b) - 1) n = sizeof(b) - 1;
  if ((int)Serial.availableForWrite() >= n) Serial.write((const uint8_t*)b, n);
}

// ── Status ───────────────────────────────────────────────────────
struct Status {
  bool radioOk = false; int radioErr = 0;
  bool sdOk = false;    uint64_t sdMb = 0;
  bool touchOk = false; uint8_t touchAddr = 0; int tx = -1, ty = -1; bool touching = false;
  bool kbOk = false;    char lastKey = 0; uint32_t keyCount = 0;
  uint32_t rxCount = 0, txCount = 0; int rxLen = 0; float rssi = 0, snr = 0;
  uint32_t loopMaxMs = 0;
} st;

static uint32_t clickSeen = 0;

// Non-blocking transmit: start, then finish when DIO1 fires (or after a timeout).
static bool     txBusy = false;
static uint32_t txStartMs = 0, txLastMs = 0, txTimeouts = 0;
static int16_t  txLastErr = 0;
static constexpr uint32_t TX_TIMEOUT_MS = 2000;

// Worst-case time spent in each part of the loop, to name any stall.
enum { T_TOUCH, T_KEY, T_RADIO, T_DRAW, T_CLICK, T_LOG, T_COUNT };
static uint32_t worstMs[T_COUNT];
static const char* const worstName[T_COUNT] = {"touch", "key", "radio", "draw", "click", "log"};

// ── GT911 touch (STOP+START reads; repeated start is unreliable on ESP32) ──
static bool gt911Read(uint16_t reg, uint8_t* buf, uint8_t n) {
  Wire.beginTransmission(st.touchAddr);
  Wire.write(reg >> 8); Wire.write(reg & 0xFF);
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom(st.touchAddr, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}
static void gt911Clear() {
  Wire.beginTransmission(st.touchAddr);
  Wire.write(0x81); Wire.write(0x4E); Wire.write(0x00);
  Wire.endTransmission(true);
}
static void pollTouch() {
  if (!st.touchOk) return;
  uint8_t s;
  if (!gt911Read(0x814E, &s, 1)) return;
  if (s & 0x80) {
    uint8_t n = s & 0x0F;
    if (n > 0) {
      uint8_t p[4];
      if (gt911Read(0x8150, p, 4)) {
        int px = p[0] | (p[1] << 8), py = p[2] | (p[3] << 8);
        st.tx = py;                 // portrait panel -> landscape (rotation 1)
        st.ty = 239 - px;
        st.touching = true;
      }
    } else {
      st.touching = false;
    }
    gt911Clear();
  }
}

static void pollKeyboard() {
  if (!st.kbOk) return;
  if (Wire.requestFrom(KB_ADDR, (uint8_t)1) == 1) {
    char c = Wire.read();
    if (c) {
      st.lastKey = c; st.keyCount++;
      if ((c == 't' || c == 'T') && !txBusy && st.radioOk) {
        radio->setOutputPower(LORA_TX_DBM);
        txLastErr = radio->startTransmit("HELLO-TDECK");
        if (txLastErr == RADIOLIB_ERR_NONE) { txBusy = true; txStartMs = millis(); }
      }
    }
  }
}

static void pollRadio() {
  if (!st.radioOk) return;
  if (txBusy) {
    if (rxFlag) {                     // DIO1 = TX done
      rxFlag = false;
      radio->finishTransmit();
      st.txCount++; txLastMs = millis() - txStartMs; txBusy = false;
      radio->startReceive();
    } else if (millis() - txStartMs > TX_TIMEOUT_MS) {
      radio->finishTransmit();
      txTimeouts++; txBusy = false;
      radio->startReceive();
    }
    return;
  }
  if (!rxFlag) return;
  rxFlag = false;
  uint8_t buf[256];
  size_t len = radio->getPacketLength();
  if (len > sizeof(buf)) len = sizeof(buf);
  int16_t r = radio->readData(buf, len);
  if (r == RADIOLIB_ERR_NONE && len > 0) {
    st.rxCount++; st.rxLen = (int)len; st.rssi = radio->getRSSI(); st.snr = radio->getSNR();
  }
  radio->startReceive();
}

static uint16_t battMv() {
  analogReadResolution(12);
  uint32_t raw = 0;
  for (int i = 0; i < 8; i++) raw += analogRead(PIN_BATT);
  return (uint16_t)((2.0f * 3.3f * 1000.0f * (raw / 8)) / 4096.0f);
}

// ── Screen ───────────────────────────────────────────────────────
static int row = 0;
static void line(uint16_t color, const char* fmt, ...) {
  char b[64];
  va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextPadding(320);
  tft.drawString(b, 4, 4 + row * 18, 2);
  row++;
}
static void drawStatus() {
  row = 0;
  line(TFT_CYAN, "T-Deck hello board  up %lus", (unsigned long)(millis() / 1000));
  line(st.radioOk ? TFT_GREEN : TFT_RED, "Radio %s  %.3f MHz SF%u BW%.1f", st.radioOk ? "OK" : "FAIL",
       LORA_FREQ, LORA_SF, LORA_BW);
  line(TFT_WHITE, "RX %lu len %d rssi %.0f snr %.1f", (unsigned long)st.rxCount, st.rxLen, st.rssi, st.snr);
  line(txTimeouts ? TFT_ORANGE : TFT_WHITE, "TX %lu  %lu ms  timeouts %lu  err %d%s", (unsigned long)st.txCount,
       (unsigned long)txLastMs, (unsigned long)txTimeouts, (int)txLastErr, txBusy ? "  SENDING" : "");
  line(st.touchOk ? TFT_GREEN : TFT_RED, "Touch %s 0x%02X  %s x=%d y=%d", st.touchOk ? "OK" : "none",
       st.touchAddr, st.touching ? "DOWN" : "up", st.tx, st.ty);
  line(st.kbOk ? TFT_GREEN : TFT_RED, "Keyboard %s  last '%c'  n=%lu", st.kbOk ? "OK" : "none",
       st.lastKey ? st.lastKey : '-', (unsigned long)st.keyCount);
  line(TFT_YELLOW, "Ball U%lu D%lu L%lu R%lu  CLICK %lu", (unsigned long)isrUp, (unsigned long)isrDown,
       (unsigned long)isrLeft, (unsigned long)isrRight, (unsigned long)isrClick);
  line(st.sdOk ? TFT_GREEN : TFT_ORANGE, "SD %s  %llu MB", st.sdOk ? "OK" : "no card", (unsigned long long)st.sdMb);
  line(TFT_WHITE, "Batt %u mV  heap %u  psram %u", battMv(), (unsigned)ESP.getFreeHeap(),
       (unsigned)ESP.getFreePsram());
  line(TFT_WHITE, "Loop max %lu ms", (unsigned long)st.loopMaxMs);
  line(TFT_DARKGREY, "press 't' to send a test frame");
}

static void flashClick() {
  tft.fillRect(300, 4, 16, 16, TFT_MAGENTA);
  delay(80);
  tft.fillRect(300, 4, 16, 16, TFT_BLACK);
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(1);          // keep any unavoidable wait tiny; logf() avoids it anyway

  pinMode(PIN_POWERON, OUTPUT); digitalWrite(PIN_POWERON, HIGH);
  delay(150);
  // Keep every SPI device deselected before the bus is used.
  pinMode(PIN_LORA_CS, OUTPUT); digitalWrite(PIN_LORA_CS, HIGH);
  pinMode(PIN_SD_CS, OUTPUT);   digitalWrite(PIN_SD_CS, HIGH);
  pinMode(PIN_TFT_CS, OUTPUT);  digitalWrite(PIN_TFT_CS, HIGH);

  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  pinMode(42, OUTPUT); digitalWrite(42, HIGH);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("booting...", 4, 4, 2);

  // Trackball: directions count edges, click is latched with a 20 ms debounce.
  const int dirs[4] = {PIN_TB_UP, PIN_TB_DOWN, PIN_TB_LEFT, PIN_TB_RIGHT};
  void (*isrs[4])() = {onUp, onDown, onLeft, onRight};
  for (int i = 0; i < 4; i++) { pinMode(dirs[i], INPUT_PULLUP); attachInterrupt(dirs[i], isrs[i], FALLING); }
  pinMode(PIN_TB_CLICK, INPUT_PULLUP);
  attachInterrupt(PIN_TB_CLICK, onClick, FALLING);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);
  Wire.beginTransmission(KB_ADDR);
  st.kbOk = (Wire.endTransmission(true) == 0);
  for (uint8_t a : {(uint8_t)0x5D, (uint8_t)0x14}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission(true) == 0) { st.touchOk = true; st.touchAddr = a; break; }
  }

  // SD shares the SPI bus with the display and the radio.
  st.sdOk = SD.begin(PIN_SD_CS, TFT_eSPI::getSPIinstance(), 4000000);
  if (st.sdOk) st.sdMb = SD.cardSize() / (1024ULL * 1024ULL);

  radio = new SX1262(new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY,
                                TFT_eSPI::getSPIinstance()));
  // LoRa: MeshCore uses the private sync word 0x12 and a 16-symbol preamble.
  st.radioErr = radio->begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, 0x12, LORA_TX_DBM, 16, 1.8f, false);
  if (st.radioErr == RADIOLIB_ERR_NONE) {
    radio->setDio2AsRfSwitch(false);
    radio->setCurrentLimit(140);
    radio->setRxBoostedGainMode(true);
    radio->setCRC(2);
    radio->setPacketReceivedAction(onRadio);
    st.radioOk = (radio->startReceive() == RADIOLIB_ERR_NONE);
  }
  logf("hello: radio=%d touch=0x%02X kb=%d sd=%d\n", st.radioErr, st.touchAddr, st.kbOk, st.sdOk);
  tft.fillScreen(TFT_BLACK);
}

void loop() {
  static uint32_t lastDraw = 0, lastLog = 0;
  uint32_t t0 = millis();

  uint32_t a = millis();
  pollTouch();    worstMs[T_TOUCH] = max<uint32_t>(worstMs[T_TOUCH], millis() - a); a = millis();
  pollKeyboard(); worstMs[T_KEY]   = max<uint32_t>(worstMs[T_KEY],   millis() - a); a = millis();
  pollRadio();    worstMs[T_RADIO] = max<uint32_t>(worstMs[T_RADIO], millis() - a);
  if (isrClick != clickSeen) {
    clickSeen = isrClick;
    a = millis(); flashClick(); worstMs[T_CLICK] = max<uint32_t>(worstMs[T_CLICK], millis() - a);
  }

  uint32_t now = millis();
  if (now - lastDraw >= 250) {
    lastDraw = now;
    a = millis(); drawStatus(); worstMs[T_DRAW] = max<uint32_t>(worstMs[T_DRAW], millis() - a);
  }
  if (now - lastLog >= 5000) {
    lastLog = now;
    a = millis();
    logf("ball U%lu D%lu L%lu R%lu clickraw=%lu click=%lu\n", (unsigned long)isrUp, (unsigned long)isrDown,
                  (unsigned long)isrLeft, (unsigned long)isrRight, (unsigned long)isrClickRaw, (unsigned long)isrClick);
    logf("up=%lus rx=%lu tx=%lu txms=%lu click=%lu heap=%u loopmax=%lums worst[", (unsigned long)(now / 1000),
                  (unsigned long)st.rxCount, (unsigned long)st.txCount, (unsigned long)txLastMs,
                  (unsigned long)isrClick, (unsigned)ESP.getFreeHeap(), (unsigned long)st.loopMaxMs);
    for (int i = 0; i < T_COUNT; i++) logf("%s=%lu ", worstName[i], (unsigned long)worstMs[i]);
    logf("]\n");
    worstMs[T_LOG] = max<uint32_t>(worstMs[T_LOG], millis() - a);
  }
  uint32_t dt = millis() - t0;
  if (dt > st.loopMaxMs) st.loopMaxMs = dt;
  delay(2);
}
