#pragma once
// MPU6050: Beschleunigung + Drehrate, Komplementärfilter für Nick (pitch) und
// Roll im Körpersystem (Vorzeichen wie die Körperhaltung, siehe attitude.h).
// update() läuft im Regel-Task; die übrigen Funktionen sind aus anderen Tasks
// aufrufbar (Anforderungen werden im nächsten update() übernommen).
//
// Kalibrierung (gespeichert im NVS):
//  - Einbaulage: Richtung des X-Pfeils der Platine, Bauteilseite oben/unten
//  - Gyro-Nullpunkt: beim Start nur übernommen, wenn der Roboter still stand,
//    sonst gilt der gespeicherte Wert; im Stillstand langsam nachgeführt
//  - Nullpunkt der Lage ("waagerecht"): gleicht eine schief eingebaute Platine aus

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Wire.h>

#include "attitude.h"

class Imu {
 public:
  bool begin(TwoWire& wire, uint8_t addr);
  // dt_s = Zeit seit dem letzten Aufruf
  bool update(float dt_s);
  // aus loop(): geänderte Kalibrierung im NVS speichern
  void loop();

  bool ok() const { return ok_; }
  float pitchDeg() const { return pitch_out_; }
  float rollDeg() const { return roll_out_; }
  // letzte Messung in Körper-Achsen (Beschleunigung in g, Drehrate in °/s; aus dem Regel-Task)
  void accelG(float out[3]) const { out[0] = accel_g_.x, out[1] = accel_g_.y, out[2] = accel_g_.z; }
  void gyroDps(float out[3]) const { out[0] = gyro_dps_.x, out[1] = gyro_dps_.y, out[2] = gyro_dps_.z; }

  // Kalibrierung: Roboter ca. 2 s ruhig halten. level = aktuelle Lage zusätzlich
  // als waagerecht übernehmen (sonst nur der Gyro-Nullpunkt).
  void requestCalibration(bool level);
  void resetLevel();
  void setMount(attitude::AxisDir x_dir, bool upside_down);

  void toJson(JsonObject obj) const;

 private:
  enum class CalState : uint8_t { Idle, Running, Ok, Failed };

  bool readRaw(int16_t* out7);
  bool writeReg(uint8_t reg, uint8_t val);
  bool readReg(uint8_t reg, uint8_t& val);
  void loadSettings();
  void processRequests();
  void calibrationStep(const attitude::V3& gyro_raw, const attitude::V3& acc_raw);

  TwoWire* wire_ = nullptr;
  uint8_t addr_ = 0x68;
  bool ok_ = false;
  uint8_t who_am_i_ = 0;
  uint8_t error_count_ = 0;
  uint32_t last_retry_ms_ = 0;

  // Kalibrierung (Sensor-Achsen für den Gyro, Körper-Achsen für den Nullpunkt)
  attitude::Mount mount_;
  float gyro_bias_[3] = {0, 0, 0};
  bool bias_valid_ = false;
  float level_roll_ = 0.0f, level_pitch_ = 0.0f;
  bool settings_loaded_ = false;
  bool begun_ = false;
  const char* boot_bias_ = "";  // Herkunft des Gyro-Nullpunkts beim Start

  // Anforderungen aus anderen Tasks
  volatile uint8_t req_cal_ = 0;  // 1 = Gyro, 2 = Gyro + waagerecht
  volatile bool req_level_reset_ = false;
  volatile bool req_mount_ = false;
  volatile uint8_t req_mount_dir_ = 0;
  volatile bool req_mount_flip_ = false;
  volatile bool save_pending_ = false;

  // laufende Kalibrierung
  volatile CalState cal_state_ = CalState::Idle;
  bool cal_level_ = false;
  attitude::Stats cal_gyro_, cal_acc_;
  const char* cal_msg_ = "";
  uint32_t cal_done_ms_ = 0;

  attitude::Filter filter_;
  bool filter_reset_ = true;
  attitude::V3 accel_g_ = {0, 0, 0};  // Körper-Achsen
  attitude::V3 gyro_dps_ = {0, 0, 0};
  float pitch_out_ = 0.0f;
  float roll_out_ = 0.0f;

  // Diagnose
  float temp_c_ = 0.0f;
  uint32_t errors_total_ = 0;
  float rate_hz_ = 0.0f;
  uint32_t rate_start_ms_ = 0;
  uint32_t rate_reads_ = 0;
};

extern Imu imu;
