#pragma once
// Statusanzeige über die beiden WS2812B-"Augen" (GPIO 14) und die WS2812 auf dem
// ESP32-S3-CAM-Board (GPIO 48). Farben und Effekte je Zustand sind einstellbar
// (Seite "LEDs", gespeichert im NVS); Rechnung in led_fx. Die Board-LED sitzt direkt
// vor der Kamera und leuchtet standardmäßig nur beim Start und Verbinden.

#include <Arduino.h>
#include <ArduinoJson.h>

#include "led_fx.h"

class StatusLeds {
 public:
  void begin();  // lädt die Einstellungen
  void setState(ledfx::State s) { state_ = s; }
  ledfx::State state() const { return state_; }
  void off();
  // Zyklisch aufrufen (ca. 25 Hz): Effekt rechnen und schreiben, geänderte
  // Einstellungen verzögert speichern
  void update();

  // Augen kurz vorgeben (Spielereien), nicht über Warnungen; ms = 0 -> wieder automatisch
  void showOverride(const ledfx::Style& s, uint32_t ms);
  bool overrideStyle(ledfx::Style& out);  // true, solange eine Vorgabe bzw. Vorschau gilt

  // Einstellungen (aus dem Web-Task)
  void configToJson(JsonObject obj);
  bool applyJson(JsonVariantConst in, String& err);

 private:
  void write(const ledfx::Rgb eyes[2], const ledfx::Rgb& board);
  void load();
  void save();

  ledfx::State state_ = ledfx::State::Boot;
  ledfx::Config cfg_ = ledfx::defaults();
  ledfx::Style preview_;
  uint32_t preview_until_ = 0;   // millis(), 0 = keine Vorschau (auch für showOverride)
  uint32_t dirty_ms_ = 0;        // letzte Änderung (0 = gespeichert)
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  ledfx::Rgb last_[2];
  ledfx::Rgb last_board_;
  bool written_ = false;
  bool ok_ = false;
};

extern StatusLeds statusLeds;
