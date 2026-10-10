#include "battery.h"

#include "config.h"

BatteryMonitor battery;

namespace {
constexpr int SAMPLES = 8;
constexpr float FILTER_ALPHA = 0.05f;  // bei 10 Hz ca. 2 s Zeitkonstante
constexpr float HYSTERESIS_V = 0.15f;

// Leerlaufkennlinie einer Li-Ion-Zelle (grob), Spannung -> Prozent
struct CurvePoint {
  float v;
  int pct;
};
constexpr CurvePoint CELL_CURVE[] = {
    {3.20f, 0}, {3.40f, 5}, {3.50f, 12}, {3.60f, 25}, {3.70f, 40},
    {3.80f, 52}, {3.90f, 65}, {4.00f, 78}, {4.10f, 90}, {4.20f, 100},
};
}  // namespace

void BatteryMonitor::begin() {
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
  voltage_ = readVoltage();
  initialized_ = true;
}

float BatteryMonitor::readVoltage() {
  uint32_t sum_mv = 0;
  for (int i = 0; i < SAMPLES; ++i) sum_mv += analogReadMilliVolts(PIN_BATTERY_ADC);
  return (sum_mv / (float)SAMPLES) / 1000.0f * BAT_DIVIDER_RATIO;
}

bool BatteryMonitor::update() {
  float v = readVoltage();
  if (!initialized_ || fabsf(v - voltage_) > 2.0f) {
    voltage_ = v;  // Akku gerade angesteckt/abgezogen -> sofort übernehmen
    initialized_ = true;
  } else {
    voltage_ += FILTER_ALPHA * (v - voltage_);
  }

  // Zustand mit Hysterese bestimmen
  if (voltage_ < BAT_PRESENT_V) {
    state_ = State::NoBattery;
  } else if (voltage_ < BAT_CRITICAL_V) {
    state_ = State::Critical;
  } else if (voltage_ < BAT_WARN_V) {
    if (state_ != State::Critical || voltage_ > BAT_CRITICAL_V + HYSTERESIS_V) state_ = State::Warn;
  } else if (state_ == State::NoBattery || voltage_ > BAT_WARN_V + HYSTERESIS_V) {
    state_ = State::Ok;
  }

  uint32_t now = millis();
  if (state_ == State::Critical) {
    if (below_critical_since_ == 0) below_critical_since_ = now ? now : 1;
    return now - below_critical_since_ >= BAT_CRITICAL_HOLD_MS;
  }
  below_critical_since_ = 0;
  return false;
}

int BatteryMonitor::percent() const {
  if (state_ == State::NoBattery) return 0;
  float cell = voltage_ / BAT_CELLS;
  constexpr int n = sizeof(CELL_CURVE) / sizeof(CELL_CURVE[0]);
  if (cell <= CELL_CURVE[0].v) return 0;
  if (cell >= CELL_CURVE[n - 1].v) return 100;
  for (int i = 1; i < n; ++i) {
    if (cell < CELL_CURVE[i].v) {
      const CurvePoint& a = CELL_CURVE[i - 1];
      const CurvePoint& b = CELL_CURVE[i];
      return a.pct + (int)lroundf((cell - a.v) / (b.v - a.v) * (b.pct - a.pct));
    }
  }
  return 100;
}

const char* BatteryMonitor::stateName(State s) {
  switch (s) {
    case State::NoBattery: return "usb";
    case State::Ok:        return "ok";
    case State::Warn:      return "warn";
    case State::Critical:  return "critical";
  }
  return "?";
}

void BatteryMonitor::toJson(JsonObject obj) const {
  obj["v"] = roundf(voltage_ * 100.0f) / 100.0f;
  obj["pct"] = percent();
  obj["state"] = stateName(state_);
}
