#include "commands.h"

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "sys/evlog.h"
#include "sys/utf8.h"
#include "ui/status_leds.h"
#include "input/pad_control.h"
#include "control_stats.h"
#include "motion/motion.h"
#include "net/camera.h"
#include "net/web_server.h"
#include "net/wifi_manager.h"
#include "sensors/battery.h"
#include "sensors/i2c_scan.h"
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
    out["msg"] = "id außerhalb 0..17";
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
  if (strcmp(cmd, "client_log") == 0) {
    // Meldung der Oberfläche (z. B. Grund eines Verbindungsabbruchs), höchstens alle 10 s
    static uint32_t last = 0;
    static uint32_t suppressed = 0;
    const uint32_t now = millis();
    if (last && now - last < 10000) {
      ++suppressed;
      return;
    }
    char text[101];
    utf8::copy(text, sizeof(text), in["text"] | "");
    if (suppressed) EV_WARN("Oberfläche: %s (+%u weitere)", text, (unsigned)suppressed);
    else EV_WARN("Oberfläche: %s", text);
    last = now ? now : 1;
    suppressed = 0;
    return;  // ohne Antwort
  }
  if (strcmp(cmd, "walk") == 0) {
    // Joystick, ca. 10 Hz: ohne Antwort, um den Funk nicht zu fluten
    motion::walk(in["vx"] | 0.0f, in["vy"] | 0.0f, in["turn"] | 0.0f);
    return;
  }
  if (strcmp(cmd, "body") == 0) {  // Körper-Joystick, wie walk ohne Antwort
    motion::body(in["x"] | 0.0f, in["y"] | 0.0f, in["roll"] | 0.0f, in["pitch"] | 0.0f, in["yaw"] | 0.0f);
    return;
  }
  if (strcmp(cmd, "pad") == 0) {  // Gamepad im Browser, ca. 20 Hz, ohne Antwort
    pad::State s;
    s.lx = in["lx"] | 0.0f;
    s.ly = in["ly"] | 0.0f;
    s.rx = in["rx"] | 0.0f;
    s.ry = in["ry"] | 0.0f;
    s.lt = in["lt"] | 0.0f;
    s.rt = in["rt"] | 0.0f;
    s.buttons = in["b"] | 0u;
    padctl::webInput(s);
    return;
  }
  if (strcmp(cmd, "arm") == 0) {  // Vorderfüße steuern, ohne Antwort
    motion::armControl(in["lx"] | 0.0f, in["lz"] | 0.0f, in["rx"] | 0.0f, in["rz"] | 0.0f, in["reach"] | 0.0f);
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
    motion::stopWalking();
    result(out, true, "Not-Aus ausgelöst");
  } else if (strcmp(cmd, "estop_clear") == 0) {
    servos.clearEmergencyStop();
    result(out, true, "Not-Aus freigegeben (Servos bleiben aus)");

  // --- Bewegungsmodus / Körperhaltung ---
  } else if (strcmp(cmd, "mode") == 0) {
    const char* m = in["mode"] | "";
    if (strcmp(m, "pose") == 0) {
      motion::setMode(motion::Mode::Pose);
      result(out, true, "Körperhaltung (IK) aktiv");
    } else if (strcmp(m, "manual") == 0) {
      motion::setMode(motion::Mode::Manual);
      result(out, true, "Manueller Modus");
    } else {
      result(out, false, "mode muss 'pose' oder 'manual' sein");
    }
  } else if (strcmp(cmd, "pose") == 0) {
    String note;
    if (motion::applyPoseJson(in, note)) out["limited"] = true;
    result(out, true, note);
  } else if (strcmp(cmd, "pose_reset") == 0) {
    motion::resetPose();
    result(out, true, "Haltung zurückgesetzt");
  } else if (strcmp(cmd, "stand") == 0) {
    if (!servos.torqueRequested() || servos.emergencyStopActive()) {
      result(out, false, "Erst Torque einschalten");
    } else {
      motion::stand();
      result(out, true, "Steht auf");
    }
  } else if (strcmp(cmd, "sit") == 0) {
    motion::sit();
    result(out, true, "Setzt sich");
  } else if (strcmp(cmd, "stop") == 0) {
    motion::stopWalking();
    result(out, true);
  } else if (strcmp(cmd, "gait") == 0) {
    result(out, motion::applyGaitJson(in, err), err);
  } else if (strcmp(cmd, "arms") == 0) {
    bool up = in["up"] | false;
    result(out, motion::setArms(up, err), err.isEmpty() ? String(up ? "Vorderbeine heben" : "Vorderbeine absetzen") : err);
  } else if (strcmp(cmd, "wave") == 0) {
    result(out, motion::wave(err), err);
  } else if (strcmp(cmd, "gesture") == 0) {
    result(out, motion::gesture(in["name"] | "", err), err);
  } else if (strcmp(cmd, "alive") == 0) {
    bool on = in["on"] | false;
    motion::setAlive(on);
    result(out, true, on ? "Lebendig an" : "Lebendig aus");
  } else if (strcmp(cmd, "leveling") == 0) {
    bool on = in["on"] | false;
    motion::setLeveling(on);
    result(out, true, on ? "Lageausgleich an" : "Lageausgleich aus");

  // --- Lagesensor ---
  } else if (strcmp(cmd, "imu_cal") == 0) {
    bool level = in["level"] | false;
    imu.requestCalibration(level);
    result(out, true, level ? "Nullpunkt wird gemessen – Roboter 2 s ruhig und waagerecht halten"
                            : "Gyro wird gemessen – Roboter 2 s ruhig halten");
  } else if (strcmp(cmd, "imu_level_reset") == 0) {
    imu.resetLevel();
    result(out, true, "Nullpunkt zurückgesetzt");
  } else if (strcmp(cmd, "imu_mount") == 0) {
    attitude::AxisDir dir;
    if (!attitude::axisDirFromName(in["x"] | "", dir)) {
      result(out, false, "x muss vorne, links, hinten oder rechts sein");
    } else {
      imu.setMount(dir, in["upside_down"] | false);
      result(out, true, "Einbaulage gespeichert – Nullpunkt neu setzen");
    }

  // --- Sollwerte (schalten in den manuellen Modus) ---
  } else if (strcmp(cmd, "servo_us") == 0) {
    if (!validId(in, id, out)) return;
    motion::setMode(motion::Mode::Manual);
    servos.setPulseUs(id, in["us"] | 1500.0f);
    result(out, true);
  } else if (strcmp(cmd, "servo_deg") == 0) {
    if (!validId(in, id, out)) return;
    motion::setMode(motion::Mode::Manual);
    float deg = servos.setAngleDeg(id, in["deg"] | 0.0f);
    out["deg"] = deg;
    result(out, true);
  } else if (strcmp(cmd, "all_us") == 0) {
    motion::setMode(motion::Mode::Manual);
    servos.setAllPulseUs(in["us"] | 1500.0f);
    result(out, true);
  } else if (strcmp(cmd, "all_deg") == 0) {
    motion::setMode(motion::Mode::Manual);
    servos.setAllAngleDeg(in["deg"] | 0.0f);
    result(out, true);
  } else if (strcmp(cmd, "all_assembly") == 0) {
    motion::setMode(motion::Mode::Manual);
    servos.setAllAssembly();
    result(out, true);
  } else if (strcmp(cmd, "slew") == 0) {
    servos.setSlewDegPerS(in["deg_per_s"] | SERVO_SLEW_DEG_PER_S);
    motion::notifySettingsChanged();
    out["deg_per_s"] = servos.slewDegPerS();
    result(out, true);

  // --- Kalibrierung ---
  } else if (strcmp(cmd, "life") == 0) {
    // "Lebendig wirken": breathe, bored, lift, walk_fx, gaze, sleep_min (Teilmenge)
    result(out, motion::applyLifeJson(in, err), err);
  } else if (strcmp(cmd, "eyes") == 0) {
    // Augen kurz vorgeben (Spielereien): color, effect, ms (bis 120 s); reset = wieder automatisch
    if (in["reset"] | false) {
      statusLeds.showOverride(ledfx::Style(), 0);
      result(out, true);
      return;
    }
    ledfx::Style st;
    st.color = {255, 255, 255};
    if (in["color"].is<const char*>() && !ledfx::parseHex(in["color"], st.color)) {
      result(out, false, "color muss #rrggbb sein");
      return;
    }
    if (in["effect"].is<const char*>() && !ledfx::effectFromName(in["effect"], st.effect)) {
      result(out, false, "unbekannter Effekt");
      return;
    }
    const float ms = in["ms"] | 3000.0f;
    statusLeds.showOverride(st, ms >= 100.0f && ms <= 120000.0f ? (uint32_t)ms : (ms > 120000.0f ? 120000 : 100));
    result(out, true);
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
    motion::setMode(motion::Mode::Manual);  // siehe calibrationPostHandler: sonst springen die Gelenke
    ServoCal c = servos.getCal(id);
    if (in["center_us"].is<float>()) c.center_us = in["center_us"];
    if (in["us_per_deg"].is<float>()) c.us_per_deg = in["us_per_deg"];
    if (in["min_deg"].is<float>()) c.min_deg = in["min_deg"];
    if (in["max_deg"].is<float>()) c.max_deg = in["max_deg"];
    result(out, servos.setCal(id, c, err), err);
    attachCal(out, id);
  } else if (strcmp(cmd, "cal_center") == 0) {
    if (!validId(in, id, out)) return;
    motion::setMode(motion::Mode::Manual);
    result(out, servos.captureCenter(id, err), err.isEmpty() ? "Montagestellung übernommen" : err);
    attachCal(out, id);
  } else if (strcmp(cmd, "cal_ref") == 0) {
    if (!validId(in, id, out)) return;
    motion::setMode(motion::Mode::Manual);
    bool ok = servos.captureReference(id, in["deg"] | 45.0f, err);
    result(out, ok, ok ? "Referenzwinkel übernommen" : err);
    attachCal(out, id);
  } else if (strcmp(cmd, "cal_invert") == 0) {
    if (!validId(in, id, out)) return;
    motion::setMode(motion::Mode::Manual);
    servos.invertDirection(id);
    result(out, true, "Drehrichtung umgekehrt");
    attachCal(out, id);
  } else if (strcmp(cmd, "cal_save") == 0) {
    bool ok = servos.saveCalibration();
    if (ok) EV_INFO("Kalibrierung gespeichert");
    else EV_ERROR("Kalibrierung speichern fehlgeschlagen");
    result(out, ok, ok ? "Kalibrierung gespeichert" : "Speichern fehlgeschlagen");
  } else if (strcmp(cmd, "cal_reload") == 0) {
    motion::setMode(motion::Mode::Manual);
    bool ok = servos.reloadCalibration();
    result(out, true, ok ? "Gespeicherte Kalibrierung geladen" : "Keine gespeicherte Kalibrierung - Standardwerte");
  } else if (strcmp(cmd, "cal_reset") == 0) {
    motion::setMode(motion::Mode::Manual);
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
  motion::toJson(doc["motion"].to<JsonObject>());
  doc["led"] = ledfx::stateName(statusLeds.state());
  ledfx::Style eyes;
  if (statusLeds.overrideStyle(eyes)) {  // Vorgabe (Spielereien) bzw. Vorschau
    char hex[8];
    ledfx::toHex(eyes.color, hex);
    JsonObject e = doc["eyes"].to<JsonObject>();
    e["color"] = hex;
    e["effect"] = ledfx::effectName(eyes.effect);
  }
  padctl::telemetry(doc["pad"].to<JsonObject>());
  JsonObject lg = doc["log"].to<JsonObject>();
  lg["seq"] = evlog::seq();
  lg["warn"] = evlog::warnSeq();
  JsonObject w = doc["wifi"].to<JsonObject>();
  bool sta = wifi::staConnected();
  w["mode"] = wifi::isAccessPoint() ? (sta ? "ap+sta" : "ap") : "sta";
  if (sta) w["rssi"] = WiFi.RSSI();
  if (wifi::powerSave()) w["ps"] = true;  // Energiesparmodus (Bluetooth an)
  doc["heap"] = ESP.getFreeHeap();
  doc["ws"] = web::wsClientCount();
  doc["ws_drop"] = web::wsDropped();
  JsonObject ctl = doc["ctl"].to<JsonObject>();
  ctl["hz"] = roundf(control_stats::hz.load() * 10.0f) / 10.0f;
  ctl["max_us"] = control_stats::max_work_us.load();
  camera::statsToJson(doc["cam"].to<JsonObject>());
}

void buildLive(JsonDocument& doc) {
  doc["type"] = "live";
  doc["t"] = millis();
  float deg[NUM_SERVOS];
  servos.anglesDeg(deg);
  JsonArray j = doc["j"].to<JsonArray>();  // Gelenkwinkel in 0,1° (Bein * 3 + Coxa/Femur/Tibia)
  for (float d : deg) j.add((int)lroundf(d * 10.0f));
  doc["tq"] = servos.torqueRequested();
  if (imu.ok()) {
    JsonArray i = doc["i"].to<JsonArray>();  // gemessen: Rollen, Nicken in 0,1°
    i.add((int)lroundf(imu.rollDeg() * 10.0f));
    i.add((int)lroundf(imu.pitchDeg() * 10.0f));
  }
  motion::liveToJson(doc.as<JsonObject>());
}

void buildStatus(JsonDocument& doc) {
  doc["fw"] = HEXAPOD_FW_VERSION;
  doc["git"] = HEXAPOD_FW_GIT;
  doc["build"] = HEXAPOD_FW_BUILT;
  doc["ui"] = HEXAPOD_UI_VERSION;  // Kennung der eingebetteten Oberfläche
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
  i2cscan::toJson(doc["i2c"].to<JsonObject>());
  battery.toJson(doc["bat"].to<JsonObject>());
}

void requestReboot() { g_reboot = true; }
bool rebootRequested() { return g_reboot; }

}  // namespace commands
