#include "status_leds.h"

#include <math.h>
#include "config.h"

StatusLeds statusLeds;

namespace {
constexpr int EYE_COUNT = 2;
constexpr uint32_t RMT_TICK_HZ = 10000000;  // 100 ns pro Tick

// WS2812B-Bit-Timing in 100-ns-Ticks
constexpr uint16_t T1H = 8, T1L = 4, T0H = 4, T0L = 8;

uint8_t scale(uint8_t v, float k) { return (uint8_t)lroundf(v * k * LED_MAX_BRIGHTNESS / 255.0f); }
}  // namespace

void StatusLeds::begin() {
  ok_ = rmtInit(PIN_EYES_LED, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, RMT_TICK_HZ);
  if (!ok_) log_e("RMT für Augen-LEDs (GPIO %d) fehlgeschlagen", PIN_EYES_LED);
  update();
}

void StatusLeds::writeEyes(uint8_t r, uint8_t g, uint8_t b) {
  uint32_t rgb = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
  // Board-LED nur, solange das Netz noch nicht steht (sie blendet die Kamera)
  writeBoard(mode_ == Mode::Boot || mode_ == Mode::WifiConnect ? rgb : 0);
  if (rgb == last_rgb_) return;
  last_rgb_ = rgb;

  if (!ok_) return;
  rmt_data_t data[EYE_COUNT * 24];
  const uint8_t grb[3] = {g, r, b};  // WS2812B erwartet G-R-B
  int i = 0;
  for (int led = 0; led < EYE_COUNT; ++led) {
    for (uint8_t c : grb) {
      for (int bit = 7; bit >= 0; --bit) {
        bool one = c & (1 << bit);
        data[i].level0 = 1;
        data[i].duration0 = one ? T1H : T0H;
        data[i].level1 = 0;
        data[i].duration1 = one ? T1L : T0L;
        ++i;
      }
    }
  }
  rmtWrite(PIN_EYES_LED, data, RMT_SYMBOLS_OF(data), 10);
}

void StatusLeds::writeBoard(uint32_t rgb) {
  if (rgb == last_board_) return;
  last_board_ = rgb;
  rgbLedWrite(PIN_BOARD_LED, rgb >> 16 & 0xFF, rgb >> 8 & 0xFF, rgb & 0xFF);  // eine WS2812, Core-Funktion
}

void StatusLeds::update() {
  uint32_t t = millis();
  float breathe = 0.25f + 0.75f * (0.5f + 0.5f * sinf(t * 0.004f));
  float blink = ((t / 250) % 2) ? 1.0f : 0.0f;
  float slow_blink = ((t / 500) % 2) ? 1.0f : 0.15f;

  uint8_t r = 0, g = 0, b = 0;
  switch (mode_) {
    case Mode::Boot:          r = scale(255, 0.4f); g = r; b = r; break;
    case Mode::WifiConnect:   b = scale(255, slow_blink); break;
    case Mode::ApMode:        b = scale(255, breathe); g = scale(80, breathe); break;
    case Mode::Idle:          g = scale(255, 0.6f); break;
    case Mode::TorqueOn:      g = scale(255, 1.0f); b = scale(255, 1.0f); break;
    case Mode::BatteryWarn:   r = scale(255, 1.0f); g = scale(110, 1.0f); break;
    case Mode::BatteryLow:    r = scale(255, breathe); break;
    case Mode::EmergencyStop: r = scale(255, blink); break;
  }
  writeEyes(r, g, b);
}
