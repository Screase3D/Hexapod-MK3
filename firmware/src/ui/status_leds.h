#pragma once
// Statusanzeige über die beiden WS2812B-"Augen" (GPIO 14) und die
// WS2812 auf dem ESP32-S3-CAM-Board (GPIO 48). Die Board-LED sitzt direkt vor
// der Kamera und leuchtet nur beim Start und Verbinden mit dem WLAN; sobald das
// Netz steht (Heimnetz oder Access Point), bleibt sie aus.

#include <Arduino.h>

class StatusLeds {
 public:
  enum class Mode : uint8_t {
    Boot,          // weiß gedimmt
    WifiConnect,   // blau blinkend
    ApMode,        // blau atmend
    Idle,          // grün (Servos aus)
    TorqueOn,      // cyan
    BatteryWarn,   // orange
    BatteryLow,    // rot atmend
    EmergencyStop  // rot blinkend
  };

  void begin();
  void setMode(Mode m) { mode_ = m; }
  void off() { writeEyes(0, 0, 0); }
  Mode mode() const { return mode_; }
  // Zyklisch aufrufen (ca. 20-50 Hz) für Animationen
  void update();

 private:
  void writeEyes(uint8_t r, uint8_t g, uint8_t b);
  void writeBoard(uint32_t rgb);

  Mode mode_ = Mode::Boot;
  uint32_t last_rgb_ = 0xFFFFFFFF;
  uint32_t last_board_ = 0xFFFFFFFF;
  bool ok_ = false;
};

extern StatusLeds statusLeds;
