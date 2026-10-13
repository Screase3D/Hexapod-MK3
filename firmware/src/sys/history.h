#pragma once
// Verlauf der letzten 30 Minuten (alle 10 s ein Wert): Akkuspannung, WLAN-Signal,
// Chip-Temperatur und freier Speicher – für die Systemübersicht (/api/history).
// Ring ist reine Logik (Test: test_host/history_test.cpp).

#include <stdint.h>

namespace history {

constexpr uint32_t PERIOD_MS = 10000;
constexpr int SAMPLES = 180;  // 30 min

struct Sample {
  uint16_t bat_mv;   // 0 = unbekannt
  int8_t rssi;       // dBm, 0 = nicht verbunden
  int8_t temp_c;     // Chip-Temperatur, -128 = unbekannt
  uint16_t heap_kb;  // freier interner Speicher
};

// Ringpuffer: die neuesten n Werte, älteste zuerst abrufbar
template <int N>
class Ring {
 public:
  void push(const Sample& s) {
    buf_[head_] = s;
    head_ = (head_ + 1) % N;
    if (count_ < N) ++count_;
  }
  int count() const { return count_; }
  // i = 0: ältester Wert
  const Sample& at(int i) const { return buf_[(head_ - count_ + i + N) % N]; }

 private:
  Sample buf_[N] = {};
  int head_ = 0;
  int count_ = 0;
};

}  // namespace history

#ifdef ARDUINO
#include <ArduinoJson.h>
namespace history {
void loop();                 // aus Arduino-loop(): alle PERIOD_MS einen Wert aufnehmen
void toJson(JsonObject obj);
}  // namespace history
#endif
