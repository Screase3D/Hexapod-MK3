#include "calibration.h"

#include <Preferences.h>
#include <math.h>
#include "robot_geometry.h"
#include "servo_map.h"

namespace {
constexpr const char* NVS_NAMESPACE = "hexapod";
constexpr const char* NVS_KEY = "cal";
constexpr uint32_t BLOB_MAGIC = 0x48584331;  // "HXC1"
// Version 1: center_us = Puls bei 0°; Version 2: Puls in der Montagestellung
constexpr uint16_t BLOB_VERSION = 2;
constexpr int JSON_VERSION = 2;

struct CalBlob {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  ServoCal servos[NUM_SERVOS];
};

// Version 1 speicherte in center_us den Puls bei 0° -> auf die Montagestellung
// umrechnen. Nie kalibrierte Standardwerte bleiben Standardwerte (die Hörner
// werden jetzt bei 1500 µs in der Montagestellung aufgesteckt).
void migrateV1(int id, ServoCal& c) {
  bool untouched = c.center_us == 1500.0f &&
                   fabsf(fabsf(c.us_per_deg) - SERVO_DEFAULT_US_PER_DEG) < 0.01f;
  if (!untouched) c.center_us += Calibration::refDeg(id) * c.us_per_deg;
}
}  // namespace

float Calibration::refDeg(int id) {
  switch (servoJoint(id)) {
    case JOINT_COXA:  return ASSEMBLY_POSE.coxa;
    case JOINT_FEMUR: return ASSEMBLY_POSE.femur;
    default:          return ASSEMBLY_POSE.tibia;
  }
}

ServoCal Calibration::defaultFor(int id) {
  ServoCal c;
  c.center_us = 1500.0f;
  c.us_per_deg = SERVO_DEFAULT_US_PER_DEG;
  // Grenzen um die Montagestellung herum, bis zum Einstellen eher vorsichtig
  float span = servoJoint(id) == JOINT_COXA ? 60.0f : 70.0f;
  c.min_deg = refDeg(id) - span;
  c.max_deg = refDeg(id) + span;
  return c;
}

void Calibration::setDefaults() {
  for (int i = 0; i < NUM_SERVOS; ++i) cal_[i] = defaultFor(i);
}

bool Calibration::load() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) {
    setDefaults();
    return false;
  }
  CalBlob blob;
  size_t len = prefs.getBytes(NVS_KEY, &blob, sizeof(blob));
  prefs.end();
  if (len != sizeof(blob) || blob.magic != BLOB_MAGIC ||
      (blob.version != 1 && blob.version != BLOB_VERSION) || blob.count != NUM_SERVOS) {
    setDefaults();
    return false;
  }
  String err;
  for (int i = 0; i < NUM_SERVOS; ++i) {
    if (blob.version == 1) migrateV1(i, blob.servos[i]);
    if (!validate(i, blob.servos[i], err)) {
      setDefaults();
      return false;
    }
  }
  memcpy(cal_, blob.servos, sizeof(cal_));
  return true;
}

bool Calibration::save() const {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  CalBlob blob;
  blob.magic = BLOB_MAGIC;
  blob.version = BLOB_VERSION;
  blob.count = NUM_SERVOS;
  memcpy(blob.servos, cal_, sizeof(cal_));
  size_t written = prefs.putBytes(NVS_KEY, &blob, sizeof(blob));
  prefs.end();
  return written == sizeof(blob);
}

float Calibration::clampDeg(int id, float deg) const {
  const ServoCal& c = cal_[id];
  if (deg < c.min_deg) return c.min_deg;
  if (deg > c.max_deg) return c.max_deg;
  return deg;
}

float Calibration::degToUs(int id, float deg) const {
  const ServoCal& c = cal_[id];
  return c.center_us + (clampDeg(id, deg) - refDeg(id)) * c.us_per_deg;
}

float Calibration::usToDeg(int id, float us) const {
  const ServoCal& c = cal_[id];
  return refDeg(id) + (us - c.center_us) / c.us_per_deg;
}

bool Calibration::validate(int id, const ServoCal& c, String& err) {
  if (!isfinite(c.center_us) || !isfinite(c.us_per_deg) ||
      !isfinite(c.min_deg) || !isfinite(c.max_deg)) {
    err = "ungültige Zahl";
    return false;
  }
  if (c.center_us < SERVO_ABS_MIN_US || c.center_us > SERVO_ABS_MAX_US) {
    err = "center_us außerhalb 500..2500";
    return false;
  }
  float slope = fabsf(c.us_per_deg);
  if (slope < 4.0f || slope > 25.0f) {
    err = "us_per_deg muss betragsmäßig zwischen 4 und 25 liegen";
    return false;
  }
  float ref = refDeg(id);
  if (c.min_deg >= ref || c.max_deg <= ref || c.min_deg < -135.0f || c.max_deg > 135.0f) {
    err = "Grenzen müssen -135 <= min < " + String(ref, 0) + " (Montagestellung) < max <= 135 erfüllen";
    return false;
  }
  return true;
}

void Calibration::toJson(JsonObject obj, int id) const {
  const ServoCal& c = cal_[id];
  obj["id"] = id;
  obj["leg"] = servoLeg(id);
  obj["joint"] = servoJoint(id);
  obj["board"] = SERVO_MAP[id].board;
  obj["channel"] = SERVO_MAP[id].channel;
  obj["ref_deg"] = refDeg(id);
  obj["center_us"] = roundf(c.center_us * 10.0f) / 10.0f;
  obj["us_per_deg"] = roundf(c.us_per_deg * 1000.0f) / 1000.0f;
  obj["min_deg"] = c.min_deg;
  obj["max_deg"] = c.max_deg;
}

void Calibration::toJson(JsonDocument& doc) const {
  doc["version"] = JSON_VERSION;
  JsonArray arr = doc["servos"].to<JsonArray>();
  for (int i = 0; i < NUM_SERVOS; ++i) toJson(arr.add<JsonObject>(), i);
}

bool Calibration::entryFromJson(JsonVariantConst obj, int id, int version, ServoCal& out, String& err) {
  if (!obj["center_us"].is<float>() || !obj["us_per_deg"].is<float>() ||
      !obj["min_deg"].is<float>() || !obj["max_deg"].is<float>()) {
    err = "Felder center_us/us_per_deg/min_deg/max_deg fehlen";
    return false;
  }
  out.center_us = obj["center_us"].as<float>();
  out.us_per_deg = obj["us_per_deg"].as<float>();
  out.min_deg = obj["min_deg"].as<float>();
  out.max_deg = obj["max_deg"].as<float>();
  if (version < 2) migrateV1(id, out);
  return validate(id, out, err);
}

bool Calibration::fromJson(JsonVariantConst root, String& err) {
  int version = root["version"] | 1;
  if (version < 1 || version > JSON_VERSION) {
    err = "unbekannte Version " + String(version);
    return false;
  }
  JsonArrayConst arr = root["servos"].as<JsonArrayConst>();
  if (arr.isNull() || arr.size() != NUM_SERVOS) {
    err = "erwarte Feld 'servos' mit 18 Einträgen";
    return false;
  }
  ServoCal next[NUM_SERVOS];
  bool seen[NUM_SERVOS] = {};
  for (JsonVariantConst e : arr) {
    int id = e["id"] | -1;
    if (id < 0 || id >= NUM_SERVOS || seen[id]) {
      err = "ungültige oder doppelte id";
      return false;
    }
    if (!entryFromJson(e, id, version, next[id], err)) {
      err = "Servo " + String(id) + ": " + err;
      return false;
    }
    seen[id] = true;
  }
  memcpy(cal_, next, sizeof(cal_));
  return true;
}
