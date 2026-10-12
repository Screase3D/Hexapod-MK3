#include "motion.h"

#include <Preferences.h>
#include <freertos/FreeRTOS.h>

#include "config.h"
#include "kinematics/gait.h"
#include "kinematics/kinematics.h"
#include "kinematics/leveling.h"
#include "kinematics/posture.h"
#include "robot_geometry.h"
#include "sensors/imu.h"
#include "servo/servo_controller.h"

namespace motion {
namespace {

constexpr float RAD2DEG = 57.2957795f;
// Einstellungen erst speichern, wenn sich eine Weile nichts mehr geändert hat
// (Schieber erzeugen viele Änderungen, der Flash soll nicht unnötig altern)
constexpr uint32_t SAVE_DELAY_MS = 2000;
constexpr float LEVEL_TILT_MAX = 25.0f;  // Nutzer-Neigung + Lageausgleich zusammen

struct PoseState {
  kin::BodyPose pose;
  float stance;
};

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
// --- geschützt durch g_mux (Web-Handler <-> Regel-Task) ---
Mode g_mode = Mode::Manual;
PoseState g_target;
bool g_enter_pose = false;      // beim Wechsel in den Pose-Modus Haltung übernehmen
bool g_enter_sitting = false;   // ... und dabei in der Sitzhaltung beginnen
bool g_leave_pose = false;      // Rampen-Untergrenze im Regel-Task zurücksetzen
bool g_req_stand = false;
bool g_req_sit = false;
posture::State g_posture = posture::State::Sitting;  // für die Telemetrie
float g_walk_cmd[3] = {0, 0, 0};
uint32_t g_walk_ms = 0;         // Zeitpunkt des letzten Fahrbefehls
gait::Params g_gait_params;
uint8_t g_unreachable = 0;      // Bitmaske Beine 0..5
uint32_t g_clamped = 0;         // Bitmaske Gelenke 0..17 (an Kalibriergrenze)
bool g_leveling = false;        // Lageausgleich eingeschaltet
bool g_level_fault = false;     // ... wegen falscher Sensorrichtung abgeschaltet
bool g_level_active = false;
float g_level_corr[2] = {0, 0};  // Roll, Nick
uint32_t g_dirty_ms = 0;        // letzte Änderung an gespeicherten Einstellungen (0 = keine)
struct GaitStatus {
  bool walking;
  uint8_t swing;
  float vx, vy, wz_deg, max_speed, max_turn_deg;
  gait::Type type;
} g_status = {};

// --- nur im Regel-Task ---
PoseState g_current;
gait::Gait g_gait;
posture::Sequencer g_seq{posture::Config{SIT_HEIGHT, SIT_STANCE, SIT_RISE_S}};
bool g_settling = false;  // Übergang in die IK-Haltung läuft noch (normale Rampe)
leveling::Controller g_level;

PoseState defaults() {
  PoseState s;
  s.pose = {0.0f, 0.0f, POSE_LIMITS.height_default, 0.0f, 0.0f, 0.0f};
  s.stance = POSE_LIMITS.stance_default;
  return s;
}

PoseState sitState() {
  PoseState s;
  s.pose = {0.0f, 0.0f, SIT_HEIGHT, 0.0f, 0.0f, 0.0f};
  s.stance = SIT_STANCE;
  return s;
}

gait::Params defaultGait() {
  gait::Params p;
  p.swing_s = GAIT_LIMITS.swing_default;
  p.step_height = GAIT_LIMITS.height_default;
  p.max_stride = GAIT_LIMITS.stride_max;
  return p;
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

float approach(float cur, float target, float max_step) {
  float d = target - cur;
  if (d > max_step) return cur + max_step;
  if (d < -max_step) return cur - max_step;
  return target;
}

void neutralFeet(float stance, kin::Vec3 out[NUM_LEGS]) {
  for (int leg = 0; leg < NUM_LEGS; ++leg) out[leg] = kin::stanceFoot(LEG_MOUNTS[leg], stance);
}

// Gelenkwinkel aller Beine für die aktuelle Haltung und Fußpunkte;
// unerreichbare Beine behalten ihre bisherigen Werte (Bitmaske zurück)
uint8_t solveIk(const kin::BodyPose& pose, const gait::Gait& g, float angles[NUM_SERVOS]) {
  uint8_t unreachable = 0;
  for (int leg = 0; leg < NUM_LEGS; ++leg) {
    const kin::LegMount& mount = LEG_MOUNTS[leg];
    kin::Vec3 foot_leg = kin::bodyToLeg(mount, kin::worldToBody(pose, g.foot(leg)));
    kin::JointAngles a;
    if (kin::legIk(LEG_GEOMETRY, mount, foot_leg, a)) {
      angles[leg * 3 + 0] = a.coxa;
      angles[leg * 3 + 1] = a.femur;
      angles[leg * 3 + 2] = a.tibia;
    } else {
      unreachable |= 1 << leg;
    }
  }
  return unreachable;
}

float g_angles[NUM_SERVOS] = {};

// Reichweite für die Gangart: Coxa-Achsen im Lauf-System (Körper verschoben bzw.
// gedreht) und erlaubter Fußabstand, damit das Knie beim Anheben nicht zu weit
// einknickt. minFootRadius rechnet etwas länger -> nur bei Änderungen.
void updateReach(const PoseState& cur, float step_height) {
  static float last_z = -1.0f, last_stance = -1.0f, last_lift = -1.0f;
  static float r_min = 0.0f, r_max = 0.0f;
  if (fabsf(cur.pose.z - last_z) > 0.25f || fabsf(cur.stance - last_stance) > 0.25f ||
      step_height != last_lift) {
    last_z = cur.pose.z;
    last_stance = cur.stance;
    last_lift = step_height;
    r_min = gait::minFootRadius(LEG_GEOMETRY, cur.stance, cur.pose.z, step_height, KNEE_MIN_DEG);
    r_max = gait::maxFootRadius(LEG_GEOMETRY, cur.pose.z);
  }
  kin::Vec3 coxa[NUM_LEGS];
  for (int leg = 0; leg < NUM_LEGS; ++leg) coxa[leg] = kin::coxaWorld(cur.pose, LEG_MOUNTS[leg]);
  g_gait.setReach(coxa, r_min, r_max);
}

// ---- Gespeicherte Einstellungen (NVS): Haltung, Gang, Lageausgleich, Stellgeschwindigkeit
constexpr const char* NVS_NAMESPACE = "hexapod";
constexpr const char* NVS_KEY = "motion";
constexpr uint32_t BLOB_MAGIC = 0x48584D31;  // "HXM1"
constexpr uint16_t BLOB_VERSION = 1;

struct SettingsBlob {
  uint32_t magic;
  uint16_t version;
  uint8_t gait_type;
  uint8_t leveling;
  float x, y, z, roll, pitch, yaw, stance;
  float swing_s, step_height;
  float slew;
};

void markDirty() {
  uint32_t now = millis();
  g_dirty_ms = now ? now : 1;
}

void loadSettings() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) return;
  SettingsBlob b;
  size_t len = prefs.getBytes(NVS_KEY, &b, sizeof(b));
  prefs.end();
  if (len != sizeof(b) || b.magic != BLOB_MAGIC || b.version != BLOB_VERSION) return;
  const float vals[] = {b.x, b.y, b.z, b.roll, b.pitch, b.yaw, b.stance, b.swing_s, b.step_height, b.slew};
  for (float v : vals) {
    if (!isfinite(v)) return;
  }
  const PoseLimits& L = POSE_LIMITS;
  const GaitLimits& G = GAIT_LIMITS;
  g_target.pose.x = clampf(b.x, -L.shift_max, L.shift_max);
  g_target.pose.y = clampf(b.y, -L.shift_max, L.shift_max);
  g_target.pose.z = clampf(b.z, L.height_min, L.height_max);
  g_target.pose.roll_deg = clampf(b.roll, -L.tilt_max, L.tilt_max);
  g_target.pose.pitch_deg = clampf(b.pitch, -L.tilt_max, L.tilt_max);
  g_target.pose.yaw_deg = clampf(b.yaw, -L.yaw_max, L.yaw_max);
  g_target.stance = clampf(b.stance, L.stance_min, L.stance_max);
  if (b.gait_type < gait::NUM_TYPES) g_gait_params.type = static_cast<gait::Type>(b.gait_type);
  g_gait_params.swing_s = clampf(b.swing_s, G.swing_min, G.swing_max);
  g_gait_params.step_height = clampf(b.step_height, G.height_min, G.height_max);
  g_leveling = b.leveling != 0;
  if (b.slew > 0.0f) servos.setSlewDegPerS(b.slew);
}

void saveSettings() {
  portENTER_CRITICAL(&g_mux);
  PoseState t = g_target;
  gait::Params gp = g_gait_params;
  bool lvl = g_leveling;
  portEXIT_CRITICAL(&g_mux);
  SettingsBlob b = {};
  b.magic = BLOB_MAGIC;
  b.version = BLOB_VERSION;
  b.gait_type = static_cast<uint8_t>(gp.type);
  b.leveling = lvl ? 1 : 0;
  b.x = t.pose.x;
  b.y = t.pose.y;
  b.z = t.pose.z;
  b.roll = t.pose.roll_deg;
  b.pitch = t.pose.pitch_deg;
  b.yaw = t.pose.yaw_deg;
  b.stance = t.stance;
  b.swing_s = gp.swing_s;
  b.step_height = gp.step_height;
  b.slew = servos.slewDegPerS();
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  prefs.putBytes(NVS_KEY, &b, sizeof(b));
  prefs.end();
}

}  // namespace

void begin() {
  g_target = defaults();
  g_current = sitState();
  g_gait_params = defaultGait();
  loadSettings();  // zuletzt eingestellte Haltung, Gang, Lageausgleich, Stellgeschwindigkeit
  g_gait.setParams(g_gait_params);
  kin::Vec3 n[NUM_LEGS];
  neutralFeet(g_current.stance, n);
  g_gait.reset(n);
  g_seq.reset(posture::State::Sitting);
  // Startsollwert der Servos: Sitzhaltung. Liegt der Roboter beim Einschalten
  // von Torque auf dem Bauch, drücken die Beine ihn so nicht ruckartig hoch.
  if (solveIk(g_current.pose, g_gait, g_angles) == 0) servos.setAnglesDeg(g_angles);
}

void setMode(Mode m) {
  portENTER_CRITICAL(&g_mux);
  if (m == Mode::Pose && g_mode != Mode::Pose) {
    g_enter_pose = true;
    g_enter_sitting = false;
  }
  if (m != Mode::Pose && g_mode == Mode::Pose) {
    g_leave_pose = true;
    g_walk_cmd[0] = g_walk_cmd[1] = g_walk_cmd[2] = 0.0f;
  }
  g_mode = m;
  portEXIT_CRITICAL(&g_mux);
}

Mode mode() {
  portENTER_CRITICAL(&g_mux);
  Mode m = g_mode;
  portEXIT_CRITICAL(&g_mux);
  return m;
}

void applyPoseJson(JsonVariantConst o) {
  // JSON außerhalb des kritischen Abschnitts auswerten, nur das Kopieren sperren
  portENTER_CRITICAL(&g_mux);
  PoseState next = g_target;
  portEXIT_CRITICAL(&g_mux);

  const PoseLimits& L = POSE_LIMITS;
  kin::BodyPose& p = next.pose;
  if (o["x"].is<float>()) p.x = clampf(o["x"], -L.shift_max, L.shift_max);
  if (o["y"].is<float>()) p.y = clampf(o["y"], -L.shift_max, L.shift_max);
  if (o["z"].is<float>()) p.z = clampf(o["z"], L.height_min, L.height_max);
  if (o["roll"].is<float>()) p.roll_deg = clampf(o["roll"], -L.tilt_max, L.tilt_max);
  if (o["pitch"].is<float>()) p.pitch_deg = clampf(o["pitch"], -L.tilt_max, L.tilt_max);
  if (o["yaw"].is<float>()) p.yaw_deg = clampf(o["yaw"], -L.yaw_max, L.yaw_max);
  if (o["stance"].is<float>()) next.stance = clampf(o["stance"], L.stance_min, L.stance_max);

  portENTER_CRITICAL(&g_mux);
  g_target = next;
  markDirty();
  portEXIT_CRITICAL(&g_mux);
}

void resetPose() {
  portENTER_CRITICAL(&g_mux);
  g_target = defaults();
  markDirty();
  portEXIT_CRITICAL(&g_mux);
}

void setLeveling(bool on) {
  portENTER_CRITICAL(&g_mux);
  g_leveling = on;
  if (on) g_level_fault = false;  // neuer Versuch (Regler wird im Regel-Task zurückgesetzt)
  markDirty();
  portEXIT_CRITICAL(&g_mux);
}

void notifySettingsChanged() {
  portENTER_CRITICAL(&g_mux);
  markDirty();
  portEXIT_CRITICAL(&g_mux);
}

void loop() {
  portENTER_CRITICAL(&g_mux);
  uint32_t dirty = g_dirty_ms;
  portEXIT_CRITICAL(&g_mux);
  if (dirty == 0 || millis() - dirty < SAVE_DELAY_MS) return;
  portENTER_CRITICAL(&g_mux);
  if (g_dirty_ms == dirty) g_dirty_ms = 0;  // in der Zwischenzeit nichts Neues
  portEXIT_CRITICAL(&g_mux);
  saveSettings();
}

void stand() {
  portENTER_CRITICAL(&g_mux);
  if (g_mode != Mode::Pose) {
    g_mode = Mode::Pose;
    g_enter_pose = true;
    g_enter_sitting = true;  // vom Boden aus aufstehen
  }
  g_req_stand = true;
  g_req_sit = false;
  portEXIT_CRITICAL(&g_mux);
}

void sit() {
  portENTER_CRITICAL(&g_mux);
  if (g_mode != Mode::Pose) {
    g_mode = Mode::Pose;
    g_enter_pose = true;
    g_enter_sitting = false;
  }
  g_req_sit = true;
  g_req_stand = false;
  g_walk_cmd[0] = g_walk_cmd[1] = g_walk_cmd[2] = 0.0f;
  portEXIT_CRITICAL(&g_mux);
}

void walk(float vx, float vy, float turn) {
  setMode(Mode::Pose);
  portENTER_CRITICAL(&g_mux);
  g_walk_cmd[0] = clampf(vx, -1.0f, 1.0f);
  g_walk_cmd[1] = clampf(vy, -1.0f, 1.0f);
  g_walk_cmd[2] = clampf(turn, -1.0f, 1.0f);
  g_walk_ms = millis();
  portEXIT_CRITICAL(&g_mux);
}

void stopWalking() {
  portENTER_CRITICAL(&g_mux);
  g_walk_cmd[0] = g_walk_cmd[1] = g_walk_cmd[2] = 0.0f;
  portEXIT_CRITICAL(&g_mux);
}

bool applyGaitJson(JsonVariantConst o, String& err) {
  portENTER_CRITICAL(&g_mux);
  gait::Params next = g_gait_params;
  portEXIT_CRITICAL(&g_mux);

  if (o["type"].is<const char*>()) {
    gait::Type t;
    if (!gait::typeFromName(o["type"].as<const char*>(), t)) {
      err = "unbekannte Gangart (tripod, ripple, wave)";
      return false;
    }
    next.type = t;
  }
  const GaitLimits& L = GAIT_LIMITS;
  if (o["swing_s"].is<float>()) next.swing_s = clampf(o["swing_s"], L.swing_min, L.swing_max);
  if (o["step_height"].is<float>()) {
    next.step_height = clampf(o["step_height"], L.height_min, L.height_max);
  }

  portENTER_CRITICAL(&g_mux);
  g_gait_params = next;
  markDirty();
  portEXIT_CRITICAL(&g_mux);
  return true;
}

void update(float dt) {
  portENTER_CRITICAL(&g_mux);
  Mode m = g_mode;
  PoseState target = g_target;
  bool entering = g_enter_pose;
  bool enter_sitting = g_enter_sitting;
  bool leaving = g_leave_pose;
  bool req_stand = g_req_stand;
  bool req_sit = g_req_sit;
  bool leveling_on = g_leveling && !g_level_fault;
  g_enter_pose = g_leave_pose = g_req_stand = g_req_sit = false;
  gait::Params gp = g_gait_params;
  float cmd[3] = {g_walk_cmd[0], g_walk_cmd[1], g_walk_cmd[2]};
  uint32_t walk_ms = g_walk_ms;
  portEXIT_CRITICAL(&g_mux);

  if (leaving) servos.setSlewFloor(0.0f);
  if (m != Mode::Pose) return;

  kin::Vec3 neutral[NUM_LEGS];
  const bool powered = servos.torqueRequested() && !servos.emergencyStopActive();
  // Ohne Kraft in den Servos liegt der Roboter -> Sitzhaltung (beim nächsten
  // Einschalten fahren die Beine dorthin, statt ihn hochzudrücken)
  bool collapse = !powered && g_seq.state() != posture::State::Sitting;
  if (entering || collapse) {
    // Direkt mit der Zielhaltung starten; die Servo-Rampen sorgen für den
    // weichen Übergang aus der bisherigen Stellung.
    bool sitting = collapse || enter_sitting;
    g_current = sitting ? sitState() : target;
    g_seq.reset(sitting ? posture::State::Sitting : posture::State::Standing);
    neutralFeet(g_current.stance, neutral);
    g_gait.reset(neutral);
    servos.setSlewFloor(0.0f);
    g_settling = true;
  }
  if (req_stand && powered) g_seq.requestStand();
  if (req_sit) g_seq.requestSit();
  // Übergang abgeschlossen -> schnellere Rampe, damit die Füße den (glatten)
  // Schrittbahnen ohne Verzögerung folgen
  if (g_settling && servos.atTarget(3.0f)) {
    g_settling = false;
    servos.setSlewFloor(GAIT_SERVO_SLEW_DEG_PER_S);
  }

  // Sicherheit: ohne frische Befehle, Torque oder bei Not-Aus nicht laufen;
  // laufen nur im Stehen
  bool can_walk = !g_settling && powered && g_seq.state() == posture::State::Standing &&
                  millis() - walk_ms < WALK_TIMEOUT_MS;
  if (!can_walk) cmd[0] = cmd[1] = cmd[2] = 0.0f;

  // Haltungswechsel (sitzen/aufstehen/hinsetzen) geben Höhe und Fußabstand vor;
  // solange der Übergang in die Haltung läuft, wartet der Ablauf
  posture::Output po = g_seq.update(g_settling ? 0.0f : dt, target.pose.z, target.stance,
                                    g_current.pose.z, g_gait.idle());
  PoseState goal = target;
  if (po.active) {
    goal.pose = {0.0f, 0.0f, po.height, 0.0f, 0.0f, 0.0f};  // Körper gerade ausrichten
    goal.stance = po.stance;
  }

  // Lageausgleich: nur im Stehen/Laufen, mit gültigem Lagesensor. Sonst baut sich
  // die Korrektur langsam ab.
  if (!leveling_on) g_level.clearFault();
  const bool level_active = leveling_on && imu.ok() && powered && !po.active && !g_settling &&
                            g_seq.state() == posture::State::Standing;
  g_level.update(dt, level_active, imu.rollDeg(), imu.pitchDeg(), goal.pose.roll_deg, goal.pose.pitch_deg);
  goal.pose.roll_deg = clampf(goal.pose.roll_deg + g_level.rollDeg(), -LEVEL_TILT_MAX, LEVEL_TILT_MAX);
  goal.pose.pitch_deg = clampf(goal.pose.pitch_deg + g_level.pitchDeg(), -LEVEL_TILT_MAX, LEVEL_TILT_MAX);

  // Haltung mit begrenzter Geschwindigkeit nachführen (fließende Bewegung).
  // Höhe und Fußabstand aus dem Haltungswechsel direkt (dort schon geglättet bzw.
  // als Sprung gewollt: die Gangart setzt die Füße dann mit Schritten um).
  const float lin = POSE_SPEED_MM_PER_S * dt;
  const float ang = POSE_SPEED_DEG_PER_S * dt;
  kin::BodyPose& c = g_current.pose;
  c.x = approach(c.x, goal.pose.x, lin);
  c.y = approach(c.y, goal.pose.y, lin);
  c.z = po.active ? goal.pose.z : approach(c.z, goal.pose.z, lin);
  c.roll_deg = approach(c.roll_deg, goal.pose.roll_deg, ang);
  c.pitch_deg = approach(c.pitch_deg, goal.pose.pitch_deg, ang);
  c.yaw_deg = approach(c.yaw_deg, goal.pose.yaw_deg, ang);
  g_current.stance = po.active ? goal.stance : approach(g_current.stance, goal.stance, lin);

  // Gang: Füße im Lauf-System
  neutralFeet(g_current.stance, neutral);
  updateReach(g_current, gp.step_height);
  g_gait.setParams(gp);
  g_gait.setCommand(cmd[0], cmd[1], cmd[2]);
  g_gait.update(dt, neutral);

  // Gelenkwinkel aller Beine per IK; unerreichbare Beine behalten ihre Werte
  uint8_t unreachable = solveIk(g_current.pose, g_gait, g_angles);
  uint32_t clamped = servos.setAnglesDeg(g_angles);

  GaitStatus st;
  st.walking = !g_gait.idle();
  st.swing = g_gait.swingMask();
  st.vx = g_gait.speedX();
  st.vy = g_gait.speedY();
  st.wz_deg = g_gait.turnRate() * RAD2DEG;
  st.max_speed = g_gait.maxSpeed();
  st.max_turn_deg = g_gait.maxTurn() * RAD2DEG;
  st.type = g_gait.activeType();

  portENTER_CRITICAL(&g_mux);
  g_unreachable = unreachable;
  g_clamped = clamped;
  g_status = st;
  g_posture = g_seq.state();
  g_level_active = level_active;
  g_level_corr[0] = g_level.rollDeg();
  g_level_corr[1] = g_level.pitchDeg();
  if (g_level.fault() && g_leveling) {
    // falsche Sensorrichtung: ausschalten und melden (bleibt gemeldet bis zum Einschalten)
    g_leveling = false;
    g_level_fault = true;
    markDirty();
  }
  portEXIT_CRITICAL(&g_mux);
}

void toJson(JsonObject obj) {
  portENTER_CRITICAL(&g_mux);
  Mode m = g_mode;
  PoseState t = g_target;
  uint8_t unreachable = g_unreachable;
  uint32_t clamped = g_clamped;
  gait::Params gp = g_gait_params;
  GaitStatus st = g_status;
  posture::State ps = g_posture;
  bool lvl_on = g_leveling, lvl_fault = g_level_fault, lvl_active = g_level_active;
  float lvl_r = g_level_corr[0], lvl_p = g_level_corr[1];
  portEXIT_CRITICAL(&g_mux);

  obj["mode"] = m == Mode::Pose ? "pose" : "manual";
  if (m == Mode::Pose) obj["posture"] = posture::stateName(ps);
  JsonObject p = obj["pose"].to<JsonObject>();
  p["x"] = t.pose.x;
  p["y"] = t.pose.y;
  p["z"] = t.pose.z;
  p["roll"] = t.pose.roll_deg;
  p["pitch"] = t.pose.pitch_deg;
  p["yaw"] = t.pose.yaw_deg;
  p["stance"] = t.stance;

  JsonObject lv = obj["level"].to<JsonObject>();
  lv["on"] = lvl_on;
  lv["active"] = lvl_active;
  lv["roll"] = roundf(lvl_r * 10.0f) / 10.0f;
  lv["pitch"] = roundf(lvl_p * 10.0f) / 10.0f;
  if (lvl_fault) lv["fault"] = true;

  JsonObject g = obj["gait"].to<JsonObject>();
  g["type"] = gait::typeName(gp.type);  // gewählt (wirkt im Stillstand)
  g["swing_s"] = gp.swing_s;
  g["step_height"] = gp.step_height;
  if (m != Mode::Pose) return;
  g["active"] = gait::typeName(st.type);
  g["walking"] = st.walking;
  g["swing"] = st.swing;
  g["vx"] = roundf(st.vx);
  g["vy"] = roundf(st.vy);
  g["wz"] = roundf(st.wz_deg);
  g["max_speed"] = roundf(st.max_speed);
  g["max_turn"] = roundf(st.max_turn_deg);

  JsonArray u = obj["unreachable"].to<JsonArray>();
  JsonArray c = obj["clamped"].to<JsonArray>();
  for (int leg = 0; leg < NUM_LEGS; ++leg) {
    if (unreachable & (1 << leg)) u.add(leg);
    if (clamped & (0x7u << (leg * 3))) c.add(leg);
  }
}

void geometryToJson(JsonObject obj) {
  obj["placeholder"] = GEOMETRY_IS_PLACEHOLDER;
  JsonObject g = obj["leg"].to<JsonObject>();
  g["coxa"] = LEG_GEOMETRY.coxa;
  g["femur"] = LEG_GEOMETRY.femur;
  g["tibia"] = LEG_GEOMETRY.tibia;
  JsonArray mounts = obj["mounts"].to<JsonArray>();
  for (const auto& m : LEG_MOUNTS) {
    JsonObject o = mounts.add<JsonObject>();
    o["x"] = m.x;
    o["y"] = m.y;
    o["angle"] = m.angle_deg;
  }
  const PoseLimits& L = POSE_LIMITS;
  JsonObject lim = obj["limits"].to<JsonObject>();
  lim["height_min"] = L.height_min;
  lim["height_max"] = L.height_max;
  lim["height_default"] = L.height_default;
  lim["stance_min"] = L.stance_min;
  lim["stance_max"] = L.stance_max;
  lim["stance_default"] = L.stance_default;
  lim["shift_max"] = L.shift_max;
  lim["tilt_max"] = L.tilt_max;
  lim["yaw_max"] = L.yaw_max;
  const GaitLimits& G = GAIT_LIMITS;
  JsonObject gl = obj["gait_limits"].to<JsonObject>();
  gl["swing_min"] = G.swing_min;
  gl["swing_max"] = G.swing_max;
  gl["swing_default"] = G.swing_default;
  gl["height_min"] = G.height_min;
  gl["height_max"] = G.height_max;
  gl["height_default"] = G.height_default;
  gl["stride_max"] = G.stride_max;
  obj["knee_min"] = KNEE_MIN_DEG;
}

}  // namespace motion
