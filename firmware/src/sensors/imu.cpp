#include "imu.h"

#include <Preferences.h>
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
constexpr int   BOOT_SAMPLES      = 100;       // Gyro-Nullpunkt beim Start (ca. 0,2 s)
constexpr int   CAL_SAMPLES       = 100;       // Kalibrierung im Betrieb (2 s bei 50 Hz)
constexpr float STILL_GYRO_DPS    = 1.0f;      // Streuung, bis zu der der Roboter "still" steht
constexpr float STILL_ACC_G       = 0.02f;
constexpr float BIAS_TRACK_K      = 0.002f;    // Nachführen im Stillstand (ca. 10 s)
constexpr float LEVEL_MAX_DEG     = 30.0f;     // stärker geneigter Nullpunkt -> Einbaulage falsch?
constexpr uint32_t RETRY_PERIOD_MS = 3000;
constexpr uint32_t CAL_SHOW_MS    = 10000;     // Ergebnis so lange in der Telemetrie

constexpr const char* NVS_NAMESPACE = "hexapod";
constexpr const char* NVS_KEY = "imu";
constexpr uint32_t BLOB_MAGIC = 0x48584931;  // "HXI1"
constexpr uint16_t BLOB_VERSION = 1;

struct ImuBlob {
  uint32_t magic;
  uint16_t version;
  uint8_t x_dir;
  uint8_t upside_down;
  float level_roll, level_pitch;
  float bias[3];
  uint8_t bias_valid;
};

char g_cal_msg[96] = "";

float r1(float v) { return roundf(v * 10.0f) / 10.0f; }
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

void Imu::loadSettings() {
  settings_loaded_ = true;
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) return;
  ImuBlob b;
  size_t len = prefs.getBytes(NVS_KEY, &b, sizeof(b));
  prefs.end();
  if (len != sizeof(b) || b.magic != BLOB_MAGIC || b.version != BLOB_VERSION) return;
  if (!isfinite(b.level_roll) || !isfinite(b.level_pitch) || fabsf(b.level_roll) > LEVEL_MAX_DEG ||
      fabsf(b.level_pitch) > LEVEL_MAX_DEG) {
    return;
  }
  mount_.x_dir = static_cast<attitude::AxisDir>(b.x_dir & 3);
  mount_.upside_down = b.upside_down != 0;
  level_roll_ = b.level_roll;
  level_pitch_ = b.level_pitch;
  if (b.bias_valid && isfinite(b.bias[0]) && isfinite(b.bias[1]) && isfinite(b.bias[2])) {
    for (int a = 0; a < 3; ++a) gyro_bias_[a] = b.bias[a];
    bias_valid_ = true;
  }
}

void Imu::loop() {
  if (!save_pending_) return;
  save_pending_ = false;
  ImuBlob b = {};
  b.magic = BLOB_MAGIC;
  b.version = BLOB_VERSION;
  b.x_dir = static_cast<uint8_t>(mount_.x_dir);
  b.upside_down = mount_.upside_down ? 1 : 0;
  b.level_roll = level_roll_;
  b.level_pitch = level_pitch_;
  for (int a = 0; a < 3; ++a) b.bias[a] = gyro_bias_[a];
  b.bias_valid = bias_valid_ ? 1 : 0;
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  prefs.putBytes(NVS_KEY, &b, sizeof(b));
  prefs.end();
}

bool Imu::begin(TwoWire& wire, uint8_t addr) {
  wire_ = &wire;
  addr_ = addr;
  ok_ = false;
  last_retry_ms_ = millis();
  if (!settings_loaded_) loadSettings();

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

  // Gyro-Nullpunkt einmalig beim Start messen. Bewegt sich der Roboter dabei
  // (z. B. in der Hand), gilt der gespeicherte Wert der letzten Kalibrierung.
  // Taucht der Sensor erst später auf (Neuversuch aus dem Regel-Task), nur messen,
  // wenn es keinen gespeicherten Wert gibt – die Messung blockiert ca. 0,2 s.
  int16_t raw[7];
  const bool first_begin = !begun_;
  begun_ = true;
  if (*boot_bias_ == '\0' && !first_begin && bias_valid_) boot_bias_ = "gespeichert";
  if (*boot_bias_ == '\0') {
    attitude::Stats s;
    for (int i = 0; i < BOOT_SAMPLES; ++i) {
      if (readRaw(raw)) {
        s.add({raw[4] / GYRO_LSB_PER_DPS, raw[5] / GYRO_LSB_PER_DPS, raw[6] / GYRO_LSB_PER_DPS});
      }
      delay(2);
    }
    if (s.count() < BOOT_SAMPLES / 2) return false;
    attitude::V3 m = s.mean();
    if (s.maxStdDev() < STILL_GYRO_DPS || !bias_valid_) {
      gyro_bias_[0] = m.x;
      gyro_bias_[1] = m.y;
      gyro_bias_[2] = m.z;
      boot_bias_ = s.maxStdDev() < STILL_GYRO_DPS ? "beim Start gemessen"
                                                  : "beim Start gemessen (Roboter war nicht still)";
      bias_valid_ = true;
    } else {
      boot_bias_ = "gespeichert (Roboter war beim Start nicht still)";
    }
    log_i("IMU Gyro-Nullpunkt %s: %.2f %.2f %.2f °/s", boot_bias_, gyro_bias_[0], gyro_bias_[1],
          gyro_bias_[2]);
  }

  filter_reset_ = true;  // Startlage aus der Beschleunigung
  error_count_ = 0;
  ok_ = true;
  return true;
}

void Imu::requestCalibration(bool level) { req_cal_ = level ? 2 : 1; }

void Imu::resetLevel() { req_level_reset_ = true; }

void Imu::setMount(attitude::AxisDir x_dir, bool upside_down) {
  req_mount_dir_ = static_cast<uint8_t>(x_dir);
  req_mount_flip_ = upside_down;
  req_mount_ = true;
}

void Imu::processRequests() {
  if (req_mount_) {
    req_mount_ = false;
    mount_.x_dir = static_cast<attitude::AxisDir>(req_mount_dir_ & 3);
    mount_.upside_down = req_mount_flip_;
    // Der Nullpunkt galt für die alte Einbaulage
    level_roll_ = level_pitch_ = 0.0f;
    filter_reset_ = true;
    save_pending_ = true;
  }
  if (req_level_reset_) {
    req_level_reset_ = false;
    level_roll_ = level_pitch_ = 0.0f;
    save_pending_ = true;
  }
  if (req_cal_ && cal_state_ != CalState::Running) {
    cal_level_ = req_cal_ == 2;
    req_cal_ = 0;
    cal_gyro_.reset();
    cal_acc_.reset();
    cal_state_ = CalState::Running;
  }
  if ((cal_state_ == CalState::Ok || cal_state_ == CalState::Failed) &&
      millis() - cal_done_ms_ > CAL_SHOW_MS) {
    cal_state_ = CalState::Idle;
  }
}

void Imu::calibrationStep(const attitude::V3& gyro_raw, const attitude::V3& acc_raw) {
  cal_gyro_.add(gyro_raw);
  cal_acc_.add(acc_raw);
  if (cal_gyro_.count() < CAL_SAMPLES) return;

  cal_done_ms_ = millis();
  attitude::V3 a = cal_acc_.mean();
  float g = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
  if (cal_gyro_.maxStdDev() > STILL_GYRO_DPS || cal_acc_.maxStdDev() > STILL_ACC_G || fabsf(g - 1.0f) > 0.1f) {
    snprintf(g_cal_msg, sizeof(g_cal_msg), "Roboter hat sich bewegt – ruhig hinstellen und wiederholen");
    cal_state_ = CalState::Failed;
    return;
  }
  attitude::V3 ab = attitude::toBody(mount_, a);
  float roll = attitude::accRollDeg(ab), pitch = attitude::accPitchDeg(ab);
  if (cal_level_ && (fabsf(roll) > LEVEL_MAX_DEG || fabsf(pitch) > LEVEL_MAX_DEG)) {
    snprintf(g_cal_msg, sizeof(g_cal_msg),
             "Lage %.0f°/%.0f° zu schräg – Roboter waagerecht stellen oder Einbaulage prüfen", roll, pitch);
    cal_state_ = CalState::Failed;
    return;
  }
  attitude::V3 m = cal_gyro_.mean();
  gyro_bias_[0] = m.x;
  gyro_bias_[1] = m.y;
  gyro_bias_[2] = m.z;
  bias_valid_ = true;
  if (cal_level_) {
    level_roll_ = roll;
    level_pitch_ = pitch;
    snprintf(g_cal_msg, sizeof(g_cal_msg), "Nullpunkt gesetzt (Sensor um Roll %+.1f°, Nick %+.1f° geneigt)",
             roll, pitch);
  } else {
    snprintf(g_cal_msg, sizeof(g_cal_msg), "Gyro-Nullpunkt neu gemessen");
  }
  filter_reset_ = true;
  save_pending_ = true;
  cal_state_ = CalState::Ok;
}

bool Imu::update(float dt_s) {
  if (!wire_) return false;
  processRequests();
  if (!ok_) {
    if (cal_state_ == CalState::Running) {
      snprintf(g_cal_msg, sizeof(g_cal_msg), "Lagesensor antwortet nicht");
      cal_done_ms_ = millis();
      cal_state_ = CalState::Failed;
    }
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

  const attitude::V3 acc = {raw[0] / ACCEL_LSB_PER_G, raw[1] / ACCEL_LSB_PER_G, raw[2] / ACCEL_LSB_PER_G};
  const attitude::V3 gyro_raw = {raw[4] / GYRO_LSB_PER_DPS, raw[5] / GYRO_LSB_PER_DPS, raw[6] / GYRO_LSB_PER_DPS};
  if (cal_state_ == CalState::Running) calibrationStep(gyro_raw, acc);

  attitude::V3 gyro = {gyro_raw.x - gyro_bias_[0], gyro_raw.y - gyro_bias_[1], gyro_raw.z - gyro_bias_[2]};
  // Im Stillstand den Gyro-Nullpunkt langsam nachführen (Temperaturdrift)
  float g = sqrtf(acc.x * acc.x + acc.y * acc.y + acc.z * acc.z);
  if (cal_state_ != CalState::Running && fabsf(gyro.x) < STILL_GYRO_DPS && fabsf(gyro.y) < STILL_GYRO_DPS &&
      fabsf(gyro.z) < STILL_GYRO_DPS && fabsf(g - 1.0f) < 0.03f) {
    gyro_bias_[0] += BIAS_TRACK_K * gyro.x;
    gyro_bias_[1] += BIAS_TRACK_K * gyro.y;
    gyro_bias_[2] += BIAS_TRACK_K * gyro.z;
  }

  accel_g_ = attitude::toBody(mount_, acc);
  const attitude::V3 gyro_body = attitude::toBody(mount_, gyro);
  if (filter_reset_) {
    filter_.reset(accel_g_);
    filter_reset_ = false;
  } else {
    filter_.update(accel_g_, gyro_body, dt_s, COMPLEMENTARY_K);
  }
  roll_out_ = filter_.rollDeg() - level_roll_;
  pitch_out_ = filter_.pitchDeg() - level_pitch_;
  return true;
}

void Imu::toJson(JsonObject obj) const {
  obj["ok"] = ok_;
  obj["who"] = who_am_i_;
  obj["errors"] = errors_total_;
  obj["mount"] = attitude::axisDirName(mount_.x_dir);
  obj["flip"] = mount_.upside_down;
  JsonArray lv = obj["level"].to<JsonArray>();
  lv.add(r1(level_roll_));
  lv.add(r1(level_pitch_));
  obj["bias_src"] = boot_bias_;
  CalState cs = cal_state_;
  if (cs != CalState::Idle) {
    obj["cal"] = cs == CalState::Running ? "running" : (cs == CalState::Ok ? "ok" : "failed");
    if (cs == CalState::Running) obj["cal_pct"] = cal_gyro_.count() * 100 / CAL_SAMPLES;
    else obj["cal_msg"] = g_cal_msg;
  }
  if (!ok_) return;
  obj["pitch"] = r1(pitch_out_);
  obj["roll"] = r1(roll_out_);
  // Beschleunigung im Körpersystem: steht der Roboter waagerecht, ist z ≈ 1 g
  JsonArray a = obj["acc"].to<JsonArray>();
  a.add(roundf(accel_g_.x * 100.0f) / 100.0f);
  a.add(roundf(accel_g_.y * 100.0f) / 100.0f);
  a.add(roundf(accel_g_.z * 100.0f) / 100.0f);
  obj["hz"] = roundf(rate_hz_);
  obj["temp"] = r1(temp_c_);
}
