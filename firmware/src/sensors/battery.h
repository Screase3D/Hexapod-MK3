#pragma once
// Akkuüberwachung (2S Li-Ion ohne BMS) über Spannungsteiler an GPIO 1.
// Gemessen wird immer (Anzeige zum Prüfen des Teilers); Warnungen,
// Servo-Abschaltung und Tiefschlaf greifen nur, wenn die Überwachung
// eingeschaltet ist.

#include <Arduino.h>
#include <ArduinoJson.h>

#include "config.h"

class BatteryMonitor {
 public:
  enum class State : uint8_t { Disabled, NoBattery, Ok, Warn, Critical };

  void begin();  // lädt auch die Schalter aus dem NVS
  // Zyklisch aufrufen (z.B. alle 100 ms). Liefert true, wenn die
  // Tiefentladeschwelle lange genug unterschritten wurde.
  bool update();

  float voltage() const { return voltage_; }
  int percent() const;
  State state() const { return state_; }
  // true, wenn die Spannung lange genug unter BAT_SHUTDOWN_V lag und
  // Überwachung sowie Tiefschlaf eingeschaltet sind
  bool shutdownRequired() const { return enabled_ && deep_sleep_ && shutdown_; }

  // Schalter (persistent im NVS)
  bool enabled() const { return enabled_; }
  bool deepSleepEnabled() const { return deep_sleep_; }
  void configure(bool enabled, bool deep_sleep);
  static const char* stateName(State s);
  void toJson(JsonObject obj) const;

 private:
  float readVoltage();

  float voltage_ = 0.0f;
  bool initialized_ = false;
  volatile bool enabled_ = BAT_MONITOR_DEFAULT;
  volatile bool deep_sleep_ = BAT_DEEP_SLEEP_DEFAULT;
  State state_ = State::Disabled;
  uint32_t below_critical_since_ = 0;
  uint32_t below_shutdown_since_ = 0;
  bool shutdown_ = false;
};

extern BatteryMonitor battery;
