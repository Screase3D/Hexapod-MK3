#include "motion.h"

#include <freertos/FreeRTOS.h>

#include "config.h"
#include "kinematics/kinematics.h"
#include "robot_geometry.h"
#include "servo/servo_controller.h"

namespace motion {
namespace {

struct PoseState {
  kin::BodyPose pose;
  float stance;
};

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
Mode g_mode = Mode::Manual;
PoseState g_target;
PoseState g_current;
bool g_enter_pose = false;      // beim Wechsel in den Pose-Modus Haltung übernehmen
uint8_t g_unreachable = 0;      // Bitmaske Beine 0..5
uint32_t g_clamped = 0;         // Bitmaske Gelenke 0..17 (an Kalibriergrenze)

PoseState defaults() {
  PoseState s;
  s.pose = {0.0f, 0.0f, POSE_LIMITS.height_default, 0.0f, 0.0f, 0.0f};
  s.stance = POSE_LIMITS.stance_default;
  return s;
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

float approach(float cur, float target, float max_step) {
  float d = target - cur;
  if (d > max_step) return cur + max_step;
  if (d < -max_step) return cur - max_step;
  return target;
}

}  // namespace

void begin() {
  g_target = g_current = defaults();
}

void setMode(Mode m) {
  portENTER_CRITICAL(&g_mux);
  if (m == Mode::Pose && g_mode != Mode::Pose) g_enter_pose = true;
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
  const PoseLimits& L = POSE_LIMITS;
  portENTER_CRITICAL(&g_mux);
  kin::BodyPose& p = g_target.pose;
  if (o["x"].is<float>()) p.x = clampf(o["x"], -L.shift_max, L.shift_max);
  if (o["y"].is<float>()) p.y = clampf(o["y"], -L.shift_max, L.shift_max);
  if (o["z"].is<float>()) p.z = clampf(o["z"], L.height_min, L.height_max);
  if (o["roll"].is<float>()) p.roll_deg = clampf(o["roll"], -L.tilt_max, L.tilt_max);
  if (o["pitch"].is<float>()) p.pitch_deg = clampf(o["pitch"], -L.tilt_max, L.tilt_max);
  if (o["yaw"].is<float>()) p.yaw_deg = clampf(o["yaw"], -L.yaw_max, L.yaw_max);
  if (o["stance"].is<float>()) g_target.stance = clampf(o["stance"], L.stance_min, L.stance_max);
  portEXIT_CRITICAL(&g_mux);
}

void resetPose() {
  portENTER_CRITICAL(&g_mux);
  g_target = defaults();
  portEXIT_CRITICAL(&g_mux);
}

void update(float dt) {
  portENTER_CRITICAL(&g_mux);
  Mode m = g_mode;
  PoseState target = g_target;
  bool entering = g_enter_pose;
  g_enter_pose = false;
  portEXIT_CRITICAL(&g_mux);
  if (m != Mode::Pose) return;

  // Beim Einschalten direkt mit der Zielhaltung starten; die Servo-Rampen
  // sorgen für den weichen Übergang aus der bisherigen Stellung.
  if (entering) g_current = target;

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

  // Gelenkwinkel aller Beine per IK; unerreichbare Beine behalten ihre Werte
  static float angles[NUM_SERVOS] = {};
  uint8_t unreachable = 0;
  for (int leg = 0; leg < NUM_LEGS; ++leg) {
    const kin::LegMount& mount = LEG_MOUNTS[leg];
    kin::Vec3 foot_world = kin::stanceFoot(mount, g_current.stance);
    kin::Vec3 foot_leg = kin::bodyToLeg(mount, kin::worldToBody(g_current.pose, foot_world));
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

  portENTER_CRITICAL(&g_mux);
  g_unreachable = unreachable;
  g_clamped = clamped;
  portEXIT_CRITICAL(&g_mux);
}

void toJson(JsonObject obj) {
  portENTER_CRITICAL(&g_mux);
  Mode m = g_mode;
  PoseState t = g_target;
  uint8_t unreachable = g_unreachable;
  uint32_t clamped = g_clamped;
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
  if (m != Mode::Pose) return;
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
}

}  // namespace motion
