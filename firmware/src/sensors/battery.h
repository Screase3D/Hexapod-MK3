#pragma once
// Akkuüberwachung (2S Li-Ion ohne BMS) über Spannungsteiler an GPIO 1.

#include <Arduino.h>
#include <ArduinoJson.h>

class BatteryMonitor {
 public:
  enum class State : uint8_t { NoBattery, Ok, Warn, Critical };

  void begin();
  // Zyklisch aufrufen (z.B. alle 100 ms). Liefert true, wenn die
  // Tiefentladeschwelle lange genug unterschritten wurde.
  bool update();

  float voltage() const { return voltage_; }
  int percent() const;
  State state() const { return state_; }
  // true, wenn die Spannung lange genug unter BAT_SHUTDOWN_V lag
  bool shutdownRequired() const { return shutdown_; }
  static const char* stateName(State s);
  void toJson(JsonObject obj) const;

 private:
  float readVoltage();

  float voltage_ = 0.0f;
  bool initialized_ = false;
  State state_ = State::NoBattery;
  uint32_t below_critical_since_ = 0;
  uint32_t below_shutdown_since_ = 0;
  bool shutdown_ = false;
};

extern BatteryMonitor battery;
