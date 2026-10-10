#include "commands.h"

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "net/camera.h"
#include "net/web_server.h"
#include "net/wifi_manager.h"
#include "sensors/battery.h"
#include "sensors/imu.h"
#include "servo/servo_controller.h"

namespace commands {
namespace {

volatile bool g_reboot = false;

bool validId(JsonVariantConst in, int& id, JsonDocument& out) {
  if (!in["id"].is<int>()) {
    out["ok"] = false;
    out["msg"] = "id fehlt";
    return false;
  }
  id = in["id"];
  if (id < 0 || id >= NUM_SERVOS) {
    out["ok"] = false;
    out["msg"] = "id ausserhalb 0..17";
    return false;
  }
  return true;
}

void attachCal(JsonDocument& out, int id) {
  servos.calibrationEntryToJson(out["cal"].to<JsonObject>(), id);
}

void result(JsonDocument& out, bool ok, const String& msg = String()) {
  out["ok"] = ok;
  if (!msg.isEmpty()) out["msg"] = msg;
}

}  // namespace

void handle(JsonVariantConst in, JsonDocument& out) {
  const char* cmd = in["cmd"] | "";
  if (strcmp(cmd, "ping") == 0) {
    out["type"] = "pong";
    return;
  }

  out["type"] = "ack";
  out["cmd"] = cmd;
  String err;
  int id = -1;

  // --- Torque / Not-Aus ---
  if (strcmp(cmd, "torque") == 0) {
    bool on = in["on"] | false;
    result(out, servos.setTorque(on, &err), err);
  } else if (strcmp(cmd, "estop") == 0) {
    servos.emergencyStop();
    result(out, true, "Not-Aus ausgeloest");
  } else if (strcmp(cmd, "estop_clear") == 0) {
    servos.clearEmergencyStop();
    result(out, true, "Not-Aus freigegeben (Servos bleiben aus)");

  // --- Sollwerte ---
  } else if (strcmp(cmd, "servo_us") == 0) {
    if (!validId(in, id, out)) return;
    servos.setPulseUs(id, in["us"] | 1500.0f);
    result(out, true);
  } else if (strcmp(cmd, "servo_deg") == 0) {
    if (!validId(in, id, out)) return;
    float deg = servos.setAngleDeg(id, in["deg"] | 0.0f);
    out["deg"] = deg;
    result(out, true);
  } else if (strcmp(cmd, "all_us") == 0) {
    servos.setAllPulseUs(in["us"] | 1500.0f);
    result(out, true);
  } else if (strcmp(cmd, "all_deg") == 0) {
    servos.setAllAngleDeg(in["deg"] | 0.0f);
    result(out, true);
  } else if (strcmp(cmd, "slew") == 0) {
    servos.setSlewDegPerS(in["deg_per_s"] | SERVO_SLEW_DEG_PER_S);
    out["deg_per_s"] = servos.slewDegPerS();
    result(out, true);

  // --- Kalibrierung ---
  } else if (strcmp(cmd, "cal_get") == 0) {
    if (in["id"].is<int>()) {
      if (!validId(in, id, out)) return;
      attachCal(out, id);
    } else {
      JsonDocument cal;
      servos.calibrationToJson(cal);
      out["calibration"] = cal;
    }
    result(out, true);
  } else if (strcmp(cmd, "cal_set") == 0) {
    if (!validId(in, id, out)) return;
    ServoCal c = servos.getCal(id);
    if (in["center_us"].is<float>()) c.center_us = in["center_us"];
    if (in["us_per_deg"].is<float>()) c.us_per_deg = in["us_per_deg"];
    if (in["min_deg"].is<float>()) c.min_deg = in["min_deg"];
    if (in["max_deg"].is<float>()) c.max_deg = in["max_deg"];
    result(out, servos.setCal(id, c, err), err);
    attachCal(out, id);
  } else if (strcmp(cmd, "cal_zero") == 0) {
    if (!validId(in, id, out)) return;
    result(out, servos.captureZero(id, err), err.isEmpty() ? "Neutralstellung uebernommen" : err);
    attachCal(out, id);
  } else if (strcmp(cmd, "cal_ref") == 0) {
    if (!validId(in, id, out)) return;
    bool ok = servos.captureReference(id, in["deg"] | 45.0f, err);
    result(out, ok, ok ? "Referenzwinkel uebernommen" : err);
    attachCal(out, id);
  } else if (strcmp(cmd, "cal_invert") == 0) {
    if (!validId(in, id, out)) return;
    servos.invertDirection(id);
    result(out, true, "Drehrichtung umgekehrt");
    attachCal(out, id);
  } else if (strcmp(cmd, "cal_save") == 0) {
    bool ok = servos.saveCalibration();
    result(out, ok, ok ? "Kalibrierung gespeichert" : "Speichern fehlgeschlagen");
  } else if (strcmp(cmd, "cal_reload") == 0) {
    bool ok = servos.reloadCalibration();
    result(out, true, ok ? "Gespeicherte Kalibrierung geladen" : "Keine gespeicherte Kalibrierung - Standardwerte");
  } else if (strcmp(cmd, "cal_reset") == 0) {
    servos.resetCalibration();
    result(out, true, "Standardwerte gesetzt (noch nicht gespeichert)");
  } else {
    result(out, false, String("unbekannter Befehl: ") + cmd);
  }
}

void buildTelemetry(JsonDocument& doc) {
  doc["type"] = "telemetry";
  doc["uptime"] = millis() / 1000;
  battery.toJson(doc["bat"].to<JsonObject>());
  servos.stateToJson(doc["servo"].to<JsonObject>());
  imu.toJson(doc["imu"].to<JsonObject>());
  JsonObject w = doc["wifi"].to<JsonObject>();
  w["mode"] = wifi::isAccessPoint() ? "ap" : "sta";
  if (!wifi::isAccessPoint()) w["rssi"] = WiFi.RSSI();
  doc["heap"] = ESP.getFreeHeap();
  doc["ws"] = web::wsClientCount();
}

void buildStatus(JsonDocument& doc) {
  doc["fw"] = HEXAPOD_FW_VERSION;
  doc["build"] = __DATE__ " " __TIME__;
  doc["uptime"] = millis() / 1000;
  JsonObject chip = doc["chip"].to<JsonObject>();
  chip["model"] = ESP.getChipModel();
  chip["rev"] = ESP.getChipRevision();
  chip["cores"] = ESP.getChipCores();
  chip["mhz"] = ESP.getCpuFreqMHz();
  chip["flash"] = ESP.getFlashChipSize();
  JsonObject mem = doc["mem"].to<JsonObject>();
  mem["heap_free"] = ESP.getFreeHeap();
  mem["heap_min"] = ESP.getMinFreeHeap();
  mem["psram"] = ESP.getPsramSize();
  mem["psram_free"] = ESP.getFreePsram();
  wifi::toJson(doc["wifi"].to<JsonObject>());
  camera::toJson(doc["camera"].to<JsonObject>());
  JsonObject dev = doc["devices"].to<JsonObject>();
  dev["pca1"] = servos.boardOk(0);
  dev["pca2"] = servos.boardOk(1);
  dev["imu"] = imu.ok();
  dev["camera"] = camera::ok();
  battery.toJson(doc["bat"].to<JsonObject>());
}

void requestReboot() { g_reboot = true; }
bool rebootRequested() { return g_reboot; }

}  // namespace commands
