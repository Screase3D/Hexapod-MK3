#include "kinematics.h"

#include <cmath>

namespace kin {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg2Rad = kPi / 180.0f;
constexpr float kRad2Deg = 180.0f / kPi;

float clampUnit(float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

// Rotation um z
Vec3 rotZ(const Vec3& p, float a) {
  float c = std::cos(a), s = std::sin(a);
  return {c * p.x - s * p.y, s * p.x + c * p.y, p.z};
}

}  // namespace

float coxaSign(const LegMount& m) {
  // Tangente einer Drehung gegen den Uhrzeigersinn: (-sin a, cos a).
  // Zeigt ihre x-Komponente nach vorne, ist "+" = vorne.
  return std::sin(m.angle_deg * kDeg2Rad) > 0.0f ? -1.0f : 1.0f;
}

bool legIk(const LegGeometry& g, const LegMount& m, const Vec3& foot, JointAngles& out) {
  float horiz = std::sqrt(foot.x * foot.x + foot.y * foot.y);
  if (horiz < 1e-3f) return false;  // Fuß genau unter der Coxa-Achse: Coxa unbestimmt

  float coxa = std::atan2(foot.y, foot.x);
  float r = horiz - g.coxa;  // waagerechter Abstand Femur-Achse -> Fuß
  float z = foot.z;
  float d = std::sqrt(r * r + z * z);
  if (d > g.femur + g.tibia || d < std::fabs(g.femur - g.tibia) || d < 1e-3f) return false;

  // Femur: Winkel zur Verbindungslinie + Winkel im Dreieck (Knie oben)
  float femur = std::atan2(z, r) +
                std::acos(clampUnit((g.femur * g.femur + d * d - g.tibia * g.tibia) / (2.0f * g.femur * d)));
  // Innenwinkel am Knie zwischen Femur und Tibia; 90° entspricht Tibia = 0°
  float knee = std::acos(clampUnit((g.femur * g.femur + g.tibia * g.tibia - d * d) / (2.0f * g.femur * g.tibia)));

  out.coxa = coxaSign(m) * coxa * kRad2Deg;
  out.femur = femur * kRad2Deg;
  out.tibia = knee * kRad2Deg - 90.0f;
  return true;
}

Vec3 legFk(const LegGeometry& g, const LegMount& m, const JointAngles& a) {
  float coxa = coxaSign(m) * a.coxa * kDeg2Rad;
  float femur = a.femur * kDeg2Rad;
  float knee = (a.tibia + 90.0f) * kDeg2Rad;
  float tibia_dir = femur + knee - kPi;  // Richtung der Tibia gegen die Waagerechte
  float r = g.coxa + g.femur * std::cos(femur) + g.tibia * std::cos(tibia_dir);
  float z = g.femur * std::sin(femur) + g.tibia * std::sin(tibia_dir);
  return {r * std::cos(coxa), r * std::sin(coxa), z};
}

Vec3 bodyToLeg(const LegMount& m, const Vec3& p) {
  return rotZ({p.x - m.x, p.y - m.y, p.z}, -m.angle_deg * kDeg2Rad);
}

Vec3 legToBody(const LegMount& m, const Vec3& p) {
  Vec3 b = rotZ(p, m.angle_deg * kDeg2Rad);
  return {b.x + m.x, b.y + m.y, b.z};
}

Vec3 worldToBody(const BodyPose& pose, const Vec3& w) {
  // Körperlage R = Rz(yaw) * Ry(-pitch) * Rx(roll); p_body = R^T * (p_world - t)
  float cr = std::cos(pose.roll_deg * kDeg2Rad), sr = std::sin(pose.roll_deg * kDeg2Rad);
  float cp = std::cos(-pose.pitch_deg * kDeg2Rad), sp = std::sin(-pose.pitch_deg * kDeg2Rad);
  float cy = std::cos(pose.yaw_deg * kDeg2Rad), sy = std::sin(pose.yaw_deg * kDeg2Rad);

  float dx = w.x - pose.x, dy = w.y - pose.y, dz = w.z - pose.z;
  // R^T * Rz(-yaw)
  float x1 = cy * dx + sy * dy;
  float y1 = -sy * dx + cy * dy;
  float z1 = dz;
  // Ry(+pitch)^T ... Ry(a)^T = Ry(-a): x = c*x - s*z, z = s*x + c*z
  float x2 = cp * x1 - sp * z1;
  float z2 = sp * x1 + cp * z1;
  float y2 = y1;
  // Rx(roll)^T = Rx(-roll): y = c*y + s*z, z = -s*y + c*z
  return {x2, cr * y2 + sr * z2, -sr * y2 + cr * z2};
}

Vec3 stanceFoot(const LegMount& m, float stance) {
  float a = m.angle_deg * kDeg2Rad;
  return {m.x + stance * std::cos(a), m.y + stance * std::sin(a), 0.0f};
}

Vec3 coxaWorld(const BodyPose& pose, const LegMount& m) {
  const float yaw = pose.yaw_deg * kDeg2Rad;
  const float c = std::cos(yaw), sn = std::sin(yaw);
  return {pose.x + c * m.x - sn * m.y, pose.y + sn * m.x + c * m.y, 0.0f};
}

PoseCheck checkPose(const LegGeometry& g, const LegMount* mounts, int legs, const BodyPose& pose,
                    const Vec3* feet, const float* lo, const float* hi) {
  PoseCheck r;
  for (int leg = 0; leg < legs; ++leg) {
    JointAngles a;
    if (!legIk(g, mounts[leg], bodyToLeg(mounts[leg], worldToBody(pose, feet[leg])), a)) {
      r.unreachable |= 1u << leg;
      continue;
    }
    const float v[3] = {a.coxa, a.femur, a.tibia};
    for (int j = 0; j < 3; ++j) {
      if (v[j] < lo[leg * 3 + j] || v[j] > hi[leg * 3 + j]) r.limited |= 1u << leg;
    }
  }
  return r;
}

}  // namespace kin
