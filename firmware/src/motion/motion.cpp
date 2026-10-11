#include "motion.h"

#include <freertos/FreeRTOS.h>

#include "config.h"
#include "kinematics/gait.h"
#include "kinematics/kinematics.h"
#include "robot_geometry.h"
#include "servo/servo_controller.h"

namespace motion {
namespace {

constexpr float RAD2DEG = 57.2957795f;

struct PoseState {
  kin::BodyPose pose;
  float stance;
};

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
// --- geschützt durch g_mux (Web-Handler <-> Regel-Task) ---
Mode g_mode = Mode::Manual;
PoseState g_target;
bool g_enter_pose = false;      // beim Wechsel in den Pose-Modus Haltung übernehmen
bool g_leave_pose = false;      // Rampen-Untergrenze im Regel-Task zurücksetzen
float g_walk_cmd[3] = {0, 0, 0};
uint32_t g_walk_ms = 0;         // Zeitpunkt des letzten Fahrbefehls
gait::Params g_gait_params;
uint8_t g_unreachable = 0;      // Bitmaske Beine 0..5
uint32_t g_clamped = 0;         // Bitmaske Gelenke 0..17 (an Kalibriergrenze)
struct GaitStatus {
  bool walking;
  uint8_t swing;
  float vx, vy, wz_deg, max_speed, max_turn_deg;
  gait::Type type;
} g_status = {};

// --- nur im Regel-Task ---
PoseState g_current;
gait::Gait g_gait;
bool g_settling = false;  // Übergang in die IK-Haltung läuft noch (normale Rampe)

PoseState defaults() {
  PoseState s;
  s.pose = {0.0f, 0.0f, POSE_LIMITS.height_default, 0.0f, 0.0f, 0.0f};
  s.stance = POSE_LIMITS.stance_default;
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

}  // namespace

void begin() {
  g_target = g_current = defaults();
  g_gait_params = defaultGait();
  g_gait.setParams(g_gait_params);
  kin::Vec3 n[NUM_LEGS];
  neutralFeet(g_current.stance, n);
  g_gait.reset(n);
}

void setMode(Mode m) {
  portENTER_CRITICAL(&g_mux);
  if (m == Mode::Pose && g_mode != Mode::Pose) g_enter_pose = true;
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
  portEXIT_CRITICAL(&g_mux);
}

void resetPose() {
  portENTER_CRITICAL(&g_mux);
  g_target = defaults();
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
  portEXIT_CRITICAL(&g_mux);
  return true;
}

void update(float dt) {
  portENTER_CRITICAL(&g_mux);
  Mode m = g_mode;
  PoseState target = g_target;
  bool entering = g_enter_pose;
  bool leaving = g_leave_pose;
  g_enter_pose = g_leave_pose = false;
  gait::Params gp = g_gait_params;
  float cmd[3] = {g_walk_cmd[0], g_walk_cmd[1], g_walk_cmd[2]};
  uint32_t walk_ms = g_walk_ms;
  portEXIT_CRITICAL(&g_mux);

  if (leaving) servos.setSlewFloor(0.0f);
  if (m != Mode::Pose) return;

  kin::Vec3 neutral[NUM_LEGS];
  if (entering) {
    // Direkt mit der Zielhaltung starten; die Servo-Rampen sorgen für den
    // weichen Übergang aus der bisherigen Stellung.
    g_current = target;
    neutralFeet(g_current.stance, neutral);
    g_gait.reset(neutral);
    servos.setSlewFloor(0.0f);
    g_settling = true;
  }
  // Übergang abgeschlossen -> schnellere Rampe, damit die Füße den (glatten)
  // Schrittbahnen ohne Verzögerung folgen
  if (g_settling && servos.atTarget(3.0f)) {
    g_settling = false;
    servos.setSlewFloor(GAIT_SERVO_SLEW_DEG_PER_S);
  }

  // Sicherheit: ohne frische Befehle, Torque oder bei Not-Aus nicht laufen
  bool can_walk = !g_settling && servos.torqueRequested() && !servos.emergencyStopActive() &&
                  millis() - walk_ms < WALK_TIMEOUT_MS;
  if (!can_walk) cmd[0] = cmd[1] = cmd[2] = 0.0f;

  // Haltung mit begrenzter Geschwindigkeit nachführen (fließende Bewegung)
  const float lin = POSE_SPEED_MM_PER_S * dt;
  const float ang = POSE_SPEED_DEG_PER_S * dt;
  kin::BodyPose& c = g_current.pose;
  c.x = approach(c.x, target.pose.x, lin);
  c.y = approach(c.y, target.pose.y, lin);
  c.z = approach(c.z, target.pose.z, lin);
  c.roll_deg = approach(c.roll_deg, target.pose.roll_deg, ang);
  c.pitch_deg = approach(c.pitch_deg, target.pose.pitch_deg, ang);
  c.yaw_deg = approach(c.yaw_deg, target.pose.yaw_deg, ang);
  g_current.stance = approach(g_current.stance, target.stance, lin);

  // Gang: Füße im Lauf-System
  neutralFeet(g_current.stance, neutral);
  g_gait.setParams(gp);
  g_gait.setCommand(cmd[0], cmd[1], cmd[2]);
  g_gait.update(dt, neutral);

  // Gelenkwinkel aller Beine per IK; unerreichbare Beine behalten ihre Werte
  static float angles[NUM_SERVOS] = {};
  uint8_t unreachable = 0;
  for (int leg = 0; leg < NUM_LEGS; ++leg) {
    const kin::LegMount& mount = LEG_MOUNTS[leg];
    kin::Vec3 foot_leg = kin::bodyToLeg(mount, kin::worldToBody(g_current.pose, g_gait.foot(leg)));
    kin::JointAngles a;
    if (kin::legIk(LEG_GEOMETRY, mount, foot_leg, a)) {
      angles[leg * 3 + 0] = a.coxa;
      angles[leg * 3 + 1] = a.femur;
      angles[leg * 3 + 2] = a.tibia;
    } else {
      unreachable |= 1 << leg;
    }
  }
  uint32_t clamped = servos.setAnglesDeg(angles);

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
  portEXIT_CRITICAL(&g_mux);

  obj["mode"] = m == Mode::Pose ? "pose" : "manual";
  JsonObject p = obj["pose"].to<JsonObject>();
  p["x"] = t.pose.x;
  p["y"] = t.pose.y;
  p["z"] = t.pose.z;
  p["roll"] = t.pose.roll_deg;
  p["pitch"] = t.pose.pitch_deg;
  p["yaw"] = t.pose.yaw_deg;
  p["stance"] = t.stance;

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
}

}  // namespace motion
