#pragma once
// Verwaltet alle 18 Servos: Sollwerte, Rampen, Torque, Not-Aus und Kalibrierung.
//
// Threading: Alle öffentlichen Methoden sind thread-sicher. Der I2C-Bus wird
// ausschließlich in update() angesprochen, das zyklisch aus dem Regel-Task
// (Core 1) aufgerufen wird. Web-Handler setzen nur Sollwerte.

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "calibration.h"
#include "pca9685.h"
#include "servo_map.h"

class ServoController {
 public:
  ServoController();

  bool begin(TwoWire& wire);
  // Zyklisch aus dem Regel-Task: Rampen rechnen, PWM schreiben, Boards prüfen.
  void update(float dt_s);

  // --- Torque / Sicherheit ---------------------------------------------------
  // Einschalten erfolgt gestaffelt (ein Servo pro Zyklus) gegen Stromspitzen.
  bool setTorque(bool on, String* err = nullptr);
  bool torqueRequested();
  void emergencyStop();
  void clearEmergencyStop();
  bool emergencyStopActive();
  // Sperre durch Akku-Überwachung o.ä.; leerer Grund hebt die Sperre auf.
  void setInhibit(const char* reason);
  String inhibitReason();

  // --- Sollwerte -------------------------------------------------------------
  void setPulseUs(int id, float us);       // Rohmodus, nur absolut begrenzt
  float setAngleDeg(int id, float deg);    // kalibriert, liefert begrenzten Winkel
  void setAllPulseUs(float us);
  void setAllAngleDeg(float deg);
  void setSlewDegPerS(float v);
  float slewDegPerS();

  // --- Kalibrierung ----------------------------------------------------------
  ServoCal getCal(int id);
  bool setCal(int id, const ServoCal& c, String& err);
  bool captureZero(int id, String& err);
  bool captureReference(int id, float deg, String& err);
  void invertDirection(int id);
  bool saveCalibration();
  bool reloadCalibration();
  void resetCalibration();
  bool calibrationDirty();
  void calibrationToJson(JsonDocument& doc);
  void calibrationEntryToJson(JsonObject obj, int id);
  bool calibrationFromJson(JsonVariantConst root, String& err);

  // --- Status ----------------------------------------------------------------
  void stateToJson(JsonObject obj);
  bool boardOk(int board);

 private:
  struct Channel {
    float target_us;
    float current_us;
    bool active;  // erhält PWM (Torque gestaffelt eingeschaltet)
  };

  void lock() { xSemaphoreTake(mutex_, portMAX_DELAY); }
  void unlock() { xSemaphoreGive(mutex_); }
  static float clampUs(float us);
  void checkBoards();

  SemaphoreHandle_t mutex_;
  Pca9685* boards_[NUM_BOARDS] = {nullptr, nullptr};
  Calibration cal_;
  Channel ch_[NUM_SERVOS];

  bool torque_req_ = false;
  bool estop_ = false;
  bool cal_dirty_ = false;
  String inhibit_;
  float slew_deg_per_s_ = SERVO_SLEW_DEG_PER_S;

  uint16_t last_ticks_[NUM_BOARDS][Pca9685::kChannels];
  bool force_write_ = true;
  uint32_t last_check_ms_ = 0;
};

extern ServoController servos;
