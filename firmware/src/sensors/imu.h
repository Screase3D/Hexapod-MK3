#pragma once
// MPU6050: Beschleunigung + Drehrate, einfacher Komplementärfilter für
// Neigung (Pitch) und Rollwinkel (Roll). Wird nur aus dem Regel-Task gelesen.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Wire.h>

class Imu {
 public:
  bool begin(TwoWire& wire, uint8_t addr);
  // dt_s = Zeit seit dem letzten Aufruf
  bool update(float dt_s);

  bool ok() const { return ok_; }
  float pitchDeg() const { return pitch_; }
  float rollDeg() const { return roll_; }
  void toJson(JsonObject obj) const;

 private:
  bool readRaw(int16_t* out7);
  bool writeReg(uint8_t reg, uint8_t val);
  bool readReg(uint8_t reg, uint8_t& val);

  TwoWire* wire_ = nullptr;
  uint8_t addr_ = 0x68;
  bool ok_ = false;
  uint8_t who_am_i_ = 0;
  float gyro_bias_[3] = {0, 0, 0};
  bool bias_valid_ = false;
  float accel_g_[3] = {0, 0, 0};
  float gyro_dps_[3] = {0, 0, 0};
  float pitch_ = 0.0f;
  float roll_ = 0.0f;
  uint8_t error_count_ = 0;
  uint32_t last_retry_ms_ = 0;

  // Diagnose
  float temp_c_ = 0.0f;
  uint32_t errors_total_ = 0;
  float rate_hz_ = 0.0f;
  uint32_t rate_start_ms_ = 0;
  uint32_t rate_reads_ = 0;
};

extern Imu imu;
