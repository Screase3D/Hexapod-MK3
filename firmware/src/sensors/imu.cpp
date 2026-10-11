#include "imu.h"

#include <math.h>

Imu imu;

namespace {
constexpr uint8_t REG_SMPLRT_DIV   = 0x19;
constexpr uint8_t REG_CONFIG       = 0x1A;
constexpr uint8_t REG_GYRO_CONFIG  = 0x1B;
constexpr uint8_t REG_ACCEL_CONFIG = 0x1C;
constexpr uint8_t REG_ACCEL_XOUT_H = 0x3B;
constexpr uint8_t REG_PWR_MGMT_1   = 0x6B;
constexpr uint8_t REG_WHO_AM_I     = 0x75;

constexpr float ACCEL_LSB_PER_G   = 16384.0f;  // ±2 g
constexpr float GYRO_LSB_PER_DPS  = 65.5f;     // ±500 °/s
constexpr float COMPLEMENTARY_K   = 0.98f;
constexpr int   BIAS_SAMPLES      = 100;
constexpr float RAD2DEG           = 57.2957795f;
constexpr uint32_t RETRY_PERIOD_MS = 3000;
}  // namespace

bool Imu::writeReg(uint8_t reg, uint8_t val) {
  wire_->beginTransmission(addr_);
  wire_->write(reg);
  wire_->write(val);
  return wire_->endTransmission() == 0;
}

bool Imu::readReg(uint8_t reg, uint8_t& val) {
  wire_->beginTransmission(addr_);
  wire_->write(reg);
  if (wire_->endTransmission(false) != 0) return false;
  if (wire_->requestFrom(addr_, (uint8_t)1) != 1) return false;
  val = wire_->read();
  return true;
}

bool Imu::readRaw(int16_t* out7) {
  wire_->beginTransmission(addr_);
  wire_->write(REG_ACCEL_XOUT_H);
  if (wire_->endTransmission(false) != 0) return false;
  if (wire_->requestFrom(addr_, (uint8_t)14) != 14) return false;
  for (int i = 0; i < 7; ++i) {
    uint8_t hi = wire_->read();
    uint8_t lo = wire_->read();
    out7[i] = (int16_t)((hi << 8) | lo);
  }
  return true;
}

bool Imu::begin(TwoWire& wire, uint8_t addr) {
  wire_ = &wire;
  addr_ = addr;
  ok_ = false;
  last_retry_ms_ = millis();

  // Erst prüfen, ob überhaupt etwas antwortet (erzeugt keine Fehlermeldungen
  // im Log, anders als ein fehlgeschlagener Registerzugriff)
  wire_->beginTransmission(addr_);
  if (wire_->endTransmission() != 0) return false;

  // WHO_AM_I: MPU6050 = 0x68; Klone (MPU6500 etc.) melden andere Werte,
  // sind registerkompatibel genug -> nur protokollieren.
  if (!readReg(REG_WHO_AM_I, who_am_i_)) return false;
  if (who_am_i_ != 0x68) log_w("IMU WHO_AM_I = 0x%02X (kein Original-MPU6050?)", who_am_i_);

  if (!writeReg(REG_PWR_MGMT_1, 0x01)) return false;  // aufwecken, PLL an Gyro X
  delay(10);
  writeReg(REG_SMPLRT_DIV, 0x00);
  writeReg(REG_CONFIG, 0x03);        // DLPF ~44 Hz
  writeReg(REG_GYRO_CONFIG, 0x08);   // ±500 °/s
  writeReg(REG_ACCEL_CONFIG, 0x00);  // ±2 g

  // Gyro-Offset einmalig bestimmen (Roboter sollte beim Start ruhig stehen).
  // Bei einer späteren Neuinitialisierung nicht wiederholen, um den
  // Regel-Task nicht zu blockieren.
  int16_t raw[7];
  if (!bias_valid_) {
    float sum[3] = {0, 0, 0};
    int good = 0;
    for (int i = 0; i < BIAS_SAMPLES; ++i) {
      if (readRaw(raw)) {
        for (int a = 0; a < 3; ++a) sum[a] += raw[4 + a] / GYRO_LSB_PER_DPS;
        ++good;
      }
      delay(2);
    }
    if (good < BIAS_SAMPLES / 2) return false;
    for (int a = 0; a < 3; ++a) gyro_bias_[a] = sum[a] / good;
    bias_valid_ = true;
  }

  // Startwinkel aus der Beschleunigung
  if (!readRaw(raw)) return false;
  for (int a = 0; a < 3; ++a) accel_g_[a] = raw[a] / ACCEL_LSB_PER_G;
  roll_ = atan2f(accel_g_[1], accel_g_[2]) * RAD2DEG;
  pitch_ = atan2f(-accel_g_[0], sqrtf(accel_g_[1] * accel_g_[1] + accel_g_[2] * accel_g_[2])) * RAD2DEG;
  error_count_ = 0;
  ok_ = true;
  return true;
}

bool Imu::update(float dt_s) {
  if (!wire_) return false;
  if (!ok_) {
    // Sensor fehlt oder ist ausgefallen -> gelegentlich neu versuchen
    uint32_t now = millis();
    if (now - last_retry_ms_ >= RETRY_PERIOD_MS) begin(*wire_, addr_);
    return false;
  }

  int16_t raw[7];
  if (!readRaw(raw)) {
    ++errors_total_;
    if (++error_count_ > 10) ok_ = false;
    return false;
  }
  error_count_ = 0;

  // Leserate für die Diagnose
  ++rate_reads_;
  uint32_t now = millis();
  if (now - rate_start_ms_ >= 1000) {
    rate_hz_ = rate_reads_ * 1000.0f / (now - rate_start_ms_);
    rate_start_ms_ = now;
    rate_reads_ = 0;
  }
  temp_c_ = raw[3] / 340.0f + 36.53f;

  for (int a = 0; a < 3; ++a) {
    accel_g_[a] = raw[a] / ACCEL_LSB_PER_G;
    gyro_dps_[a] = raw[4 + a] / GYRO_LSB_PER_DPS - gyro_bias_[a];
  }

  float acc_roll = atan2f(accel_g_[1], accel_g_[2]) * RAD2DEG;
  float acc_pitch = atan2f(-accel_g_[0], sqrtf(accel_g_[1] * accel_g_[1] + accel_g_[2] * accel_g_[2])) * RAD2DEG;
  roll_ = COMPLEMENTARY_K * (roll_ + gyro_dps_[0] * dt_s) + (1.0f - COMPLEMENTARY_K) * acc_roll;
  pitch_ = COMPLEMENTARY_K * (pitch_ + gyro_dps_[1] * dt_s) + (1.0f - COMPLEMENTARY_K) * acc_pitch;
  return true;
}

void Imu::toJson(JsonObject obj) const {
  obj["ok"] = ok_;
  obj["who"] = who_am_i_;
  obj["errors"] = errors_total_;
  if (!ok_) return;
  obj["pitch"] = roundf(pitch_ * 10.0f) / 10.0f;
  obj["roll"] = roundf(roll_ * 10.0f) / 10.0f;
  // Rohwerte zur Kontrolle: liegt der Sensor flach, ist az ≈ 1 g
  JsonArray a = obj["acc"].to<JsonArray>();
  for (int i = 0; i < 3; ++i) a.add(roundf(accel_g_[i] * 100.0f) / 100.0f);
  obj["hz"] = roundf(rate_hz_);
  obj["temp"] = roundf(temp_c_ * 10.0f) / 10.0f;
}
