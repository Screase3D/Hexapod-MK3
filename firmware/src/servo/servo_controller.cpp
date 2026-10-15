#include "servo_controller.h"

#include <math.h>

namespace {
constexpr uint32_t BOARD_CHECK_PERIOD_MS = 2000;

// Pro Board belegter Kanalbereich (aus SERVO_MAP abgeleitet)
struct BoardRange {
  uint8_t first = 15;
  uint8_t last = 0;
  uint8_t count() const { return first <= last ? last - first + 1 : 0; }
};
BoardRange g_range[NUM_BOARDS];
}  // namespace

ServoController servos;

ServoController::ServoController() {
  mutex_ = xSemaphoreCreateMutex();
  for (int i = 0; i < NUM_SERVOS; ++i) {
    ch_[i] = {1500.0f, 1500.0f, false};
    uint8_t b = SERVO_MAP[i].board;
    uint8_t c = SERVO_MAP[i].channel;
    if (c < g_range[b].first) g_range[b].first = c;
    if (c > g_range[b].last) g_range[b].last = c;
  }
  memset(last_ticks_, 0, sizeof(last_ticks_));
}

bool ServoController::begin(TwoWire& wire) {
  static Pca9685 pca1(wire, PCA1_ADDR, PCA1_OSC_HZ);
  static Pca9685 pca2(wire, PCA2_ADDR, PCA2_OSC_HZ);
  boards_[0] = &pca1;
  boards_[1] = &pca2;

  bool ok = true;
  for (int b = 0; b < NUM_BOARDS; ++b) {
    if (!boards_[b]->begin(SERVO_PWM_HZ)) {
      log_e("PCA9685 0x%02X antwortet nicht", boards_[b]->address());
      ok = false;
    }
  }

  lock();
  if (!cal_.load()) log_w("Keine gültige Kalibrierung im NVS - Standardwerte aktiv");
  // Startsollwert: Montagestellung laut Kalibrierung (Servos bleiben aber aus)
  for (int i = 0; i < NUM_SERVOS; ++i) {
    ch_[i].target_us = ch_[i].current_us = cal_.degToUs(i, Calibration::refDeg(i));
    ch_[i].active = false;
  }
  force_write_ = true;
  unlock();
  last_check_ms_ = millis();
  return ok;
}

float ServoController::clampUs(float us) {
  if (us < SERVO_ABS_MIN_US) return SERVO_ABS_MIN_US;
  if (us > SERVO_ABS_MAX_US) return SERVO_ABS_MAX_US;
  return us;
}

namespace {
constexpr uint32_t GENTLE_MAX_MS = 6000;
}  // namespace

void ServoController::update(float dt_s) {
  uint16_t ticks[NUM_BOARDS][Pca9685::kChannels] = {};

  lock();
  const bool want_on = torque_req_ && !estop_ && inhibit_.isEmpty();
  if (!want_on) {
    for (auto& c : ch_) c.active = false;
  } else {
    // Gestaffeltes Einschalten: pro Zyklus ein weiterer Servo. Die echte Position ist
    // unbekannt, der Servo springt also auf den ersten Wert: die sichere Sitzhaltung (sonst
    // der Sollwert). Von dort fährt er langsam weiter zum Sollwert.
    for (int i = 0; i < NUM_SERVOS; ++i) {
      Channel& c = ch_[i];
      if (!c.active) {
        c.active = true;
        c.current_us = have_safe_ ? clampUs(cal_.degToUs(i, cal_.clampDeg(i, safe_deg_[i]))) : c.target_us;
        startGentle();
        break;
      }
    }
  }

  float slew = slew_deg_per_s_ > slew_floor_ ? slew_deg_per_s_ : slew_floor_;
  if (slew_cap_ > 0.0f && slew > slew_cap_) slew = slew_cap_;
  if (gentle_ && slew > SERVO_GENTLE_DEG_PER_S) slew = SERVO_GENTLE_DEG_PER_S;
  bool all_there = want_on;
  for (int i = 0; i < NUM_SERVOS; ++i) {
    Channel& c = ch_[i];
    if (!c.active) continue;
    float max_step = slew * fabsf(cal_[i].us_per_deg) * dt_s;
    float diff = c.target_us - c.current_us;
    if (fabsf(diff) <= max_step) {
      c.current_us = c.target_us;
    } else {
      c.current_us += diff > 0 ? max_step : -max_step;
      all_there = false;
    }
    const ServoMapEntry& m = SERVO_MAP[i];
    if (boards_[m.board]) ticks[m.board][m.channel] = boards_[m.board]->usToTicks(c.current_us);
  }
  for (const auto& c : ch_) all_there &= c.active;  // gestaffeltes Einschalten noch nicht fertig
  // alle am Ziel – oder Notbremse: bei sich ständig ändernden Zielen (z. B. Laufen) nicht
  // dauerhaft langsam bleiben; 6 s reichen bei 45 °/s für jede Bewegung
  if (all_there || (gentle_ && millis() - gentle_ms_ > GENTLE_MAX_MS)) gentle_ = false;
  unlock();

  // Ab hier nur noch I2C (ausschließlich dieser Task greift auf den Bus zu)
  uint32_t now = millis();
  if (now - last_check_ms_ >= BOARD_CHECK_PERIOD_MS) {
    last_check_ms_ = now;
    checkBoards();
  }

  for (int b = 0; b < NUM_BOARDS; ++b) {
    Pca9685* pca = boards_[b];
    if (!pca || !pca->ok()) continue;
    const BoardRange& r = g_range[b];
    if (r.count() == 0) continue;
    const uint16_t* t = &ticks[b][r.first];
    if (!force_write_ && memcmp(t, &last_ticks_[b][r.first], r.count() * sizeof(uint16_t)) == 0) {
      continue;
    }
    if (pca->writeChannels(r.first, r.count(), t)) {
      memcpy(&last_ticks_[b][r.first], t, r.count() * sizeof(uint16_t));
    }
  }
  force_write_ = false;
}

void ServoController::checkBoards() {
  for (int b = 0; b < NUM_BOARDS; ++b) {
    Pca9685* pca = boards_[b];
    if (!pca) continue;
    if (pca->ok() && pca->healthy()) continue;
    // Nicht erreichbar oder zurückgesetzt -> neu initialisieren
    if (pca->begin(SERVO_PWM_HZ)) {
      log_w("PCA9685 0x%02X neu initialisiert", pca->address());
      force_write_ = true;
    }
  }
}

// --- Torque / Sicherheit -------------------------------------------------------

bool ServoController::setTorque(bool on, String* err) {
  lock();
  bool ok = true;
  if (on && estop_) {
    ok = false;
    if (err) *err = "Not-Aus aktiv - erst freigeben";
  } else if (on && !inhibit_.isEmpty()) {
    ok = false;
    if (err) *err = "Gesperrt: " + inhibit_;
  } else {
    torque_req_ = on;
  }
  unlock();
  return ok;
}

bool ServoController::torqueRequested() {
  lock();
  bool v = torque_req_;
  unlock();
  return v;
}

void ServoController::emergencyStop() {
  lock();
  estop_ = true;
  torque_req_ = false;
  unlock();
}

void ServoController::clearEmergencyStop() {
  lock();
  estop_ = false;
  unlock();
}

bool ServoController::emergencyStopActive() {
  lock();
  bool v = estop_;
  unlock();
  return v;
}

void ServoController::setInhibit(const char* reason) {
  lock();
  inhibit_ = reason ? reason : "";
  if (!inhibit_.isEmpty()) torque_req_ = false;
  unlock();
}

String ServoController::inhibitReason() {
  lock();
  String v = inhibit_;
  unlock();
  return v;
}

// --- Sollwerte -----------------------------------------------------------------

// Ungültige Sollwerte (NaN/unendlich) werden ignoriert – die Rampe würde damit sonst
// ungebremst über die absoluten Grenzen laufen
void ServoController::setPulseUs(int id, float us) {
  if (id < 0 || id >= NUM_SERVOS || !isfinite(us)) return;
  lock();
  ch_[id].target_us = clampUs(us);
  unlock();
}

float ServoController::setAngleDeg(int id, float deg) {
  if (id < 0 || id >= NUM_SERVOS || !isfinite(deg)) return 0.0f;
  lock();
  float clamped = cal_.clampDeg(id, deg);
  ch_[id].target_us = clampUs(cal_.degToUs(id, clamped));
  unlock();
  return clamped;
}

uint32_t ServoController::setAnglesDeg(const float* deg) {
  uint32_t clamped = 0;
  lock();
  for (int i = 0; i < NUM_SERVOS; ++i) {
    if (!isfinite(deg[i])) {  // alten Sollwert behalten
      clamped |= 1u << i;
      continue;
    }
    float d = cal_.clampDeg(i, deg[i]);
    if (d != deg[i]) clamped |= 1u << i;
    ch_[i].target_us = clampUs(cal_.degToUs(i, d));
  }
  unlock();
  return clamped;
}

// "Alle …": alle Beine zugleich – langsam, sonst springt ein sitzender Roboter hoch
void ServoController::setAllPulseUs(float us) {
  if (!isfinite(us)) return;
  lock();
  for (auto& c : ch_) c.target_us = clampUs(us);
  startGentle();
  unlock();
}

void ServoController::setAllAngleDeg(float deg) {
  if (!isfinite(deg)) return;
  lock();
  for (int i = 0; i < NUM_SERVOS; ++i) ch_[i].target_us = clampUs(cal_.degToUs(i, deg));
  startGentle();
  unlock();
}

void ServoController::setAllAssembly() {
  lock();
  for (int i = 0; i < NUM_SERVOS; ++i) {
    ch_[i].target_us = clampUs(cal_.degToUs(i, Calibration::refDeg(i)));
  }
  startGentle();
  unlock();
}

void ServoController::setSlewDegPerS(float v) {
  lock();
  slew_deg_per_s_ = constrain(v, 10.0f, 1000.0f);
  unlock();
}

// (mit Lock aufrufen)
void ServoController::startGentle() {
  if (!gentle_) gentle_ms_ = millis();
  gentle_ = true;
}

void ServoController::setSafePose(const float* deg) {
  lock();
  for (int i = 0; i < NUM_SERVOS; ++i) safe_deg_[i] = deg[i];
  have_safe_ = true;
  unlock();
}

void ServoController::setSlewCap(float deg_per_s) {
  lock();
  slew_cap_ = constrain(deg_per_s, 0.0f, 1000.0f);
  unlock();
}

void ServoController::setSlewFloor(float deg_per_s) {
  lock();
  slew_floor_ = constrain(deg_per_s, 0.0f, 1000.0f);
  unlock();
}

bool ServoController::atTarget(float tol_us) {
  lock();
  bool ok = true;
  for (const auto& c : ch_) {
    if (c.active && fabsf(c.target_us - c.current_us) > tol_us) {
      ok = false;
      break;
    }
  }
  unlock();
  return ok;
}

float ServoController::slewDegPerS() {
  lock();
  float v = slew_deg_per_s_;
  unlock();
  return v;
}

// --- Kalibrierung --------------------------------------------------------------

ServoCal ServoController::getCal(int id) {
  lock();
  ServoCal c = cal_[id];
  unlock();
  return c;
}

bool ServoController::setCal(int id, const ServoCal& c, String& err) {
  if (id < 0 || id >= NUM_SERVOS) {
    err = "ungültige Servo-ID";
    return false;
  }
  if (!Calibration::validate(id, c, err)) return false;
  lock();
  cal_[id] = c;
  cal_dirty_ = true;
  unlock();
  return true;
}

bool ServoController::captureCenter(int id, String& err) {
  if (id < 0 || id >= NUM_SERVOS) {
    err = "ungültige Servo-ID";
    return false;
  }
  lock();
  cal_[id].center_us = ch_[id].target_us;
  cal_dirty_ = true;
  unlock();
  return true;
}

bool ServoController::captureReference(int id, float deg, String& err) {
  if (id < 0 || id >= NUM_SERVOS) {
    err = "ungültige Servo-ID";
    return false;
  }
  float d_deg = deg - Calibration::refDeg(id);
  if (fabsf(d_deg) < 10.0f) {
    err = "Referenzwinkel muss mindestens 10 Grad von der Montagestellung entfernt sein";
    return false;
  }
  lock();
  ServoCal c = cal_[id];
  float delta = ch_[id].target_us - c.center_us;
  bool ok = false;
  if (fabsf(delta) < 40.0f) {
    err = "Servo steht noch zu nah an der Montagestellung";
  } else {
    c.us_per_deg = delta / d_deg;
    ok = Calibration::validate(id, c, err);
    if (ok) {
      cal_[id] = c;
      cal_dirty_ = true;
    }
  }
  unlock();
  return ok;
}

void ServoController::invertDirection(int id) {
  if (id < 0 || id >= NUM_SERVOS) return;
  lock();
  cal_[id].us_per_deg = -cal_[id].us_per_deg;
  cal_dirty_ = true;
  unlock();
}

bool ServoController::saveCalibration() {
  // NVS-Schreiben dauert einige ms -> außerhalb des Locks
  lock();
  Calibration copy = cal_;
  unlock();
  bool ok = copy.save();
  if (ok) {
    lock();
    cal_dirty_ = false;
    unlock();
  }
  return ok;
}

bool ServoController::reloadCalibration() {
  Calibration loaded;
  bool ok = loaded.load();
  lock();
  cal_ = loaded;
  cal_dirty_ = false;
  unlock();
  return ok;
}

void ServoController::resetCalibration() {
  lock();
  cal_.setDefaults();
  cal_dirty_ = true;
  unlock();
}

bool ServoController::calibrationDirty() {
  lock();
  bool v = cal_dirty_;
  unlock();
  return v;
}

void ServoController::calibrationToJson(JsonDocument& doc) {
  lock();
  cal_.toJson(doc);
  doc["dirty"] = cal_dirty_;
  unlock();
}

void ServoController::calibrationEntryToJson(JsonObject obj, int id) {
  if (id < 0 || id >= NUM_SERVOS) return;
  lock();
  cal_.toJson(obj, id);
  unlock();
}

bool ServoController::calibrationFromJson(JsonVariantConst root, String& err) {
  Calibration next;
  if (!next.fromJson(root, err)) return false;
  lock();
  cal_ = next;
  cal_dirty_ = true;
  unlock();
  return true;
}

// --- Status --------------------------------------------------------------------

bool ServoController::boardOk(int board) {
  return board >= 0 && board < NUM_BOARDS && boards_[board] && boards_[board]->ok();
}

void ServoController::anglesDeg(float* deg) {
  lock();
  for (int i = 0; i < NUM_SERVOS; ++i) deg[i] = cal_.usToDeg(i, ch_[i].target_us);
  unlock();
}

void ServoController::stateToJson(JsonObject obj) {
  lock();
  int active = 0;
  for (const auto& c : ch_) active += c.active ? 1 : 0;
  obj["torque"] = torque_req_;
  obj["active"] = active;
  obj["estop"] = estop_;
  obj["inhibit"] = inhibit_;
  obj["slew"] = slew_deg_per_s_;
  obj["cal_dirty"] = cal_dirty_;
  JsonArray us = obj["us"].to<JsonArray>();
  JsonArray deg = obj["deg"].to<JsonArray>();
  for (int i = 0; i < NUM_SERVOS; ++i) {
    us.add((int)lroundf(ch_[i].target_us));
    deg.add(roundf(cal_.usToDeg(i, ch_[i].target_us) * 10.0f) / 10.0f);
  }
  unlock();
  JsonArray boards = obj["boards"].to<JsonArray>();
  for (int b = 0; b < NUM_BOARDS; ++b) boards.add(boardOk(b));
}
