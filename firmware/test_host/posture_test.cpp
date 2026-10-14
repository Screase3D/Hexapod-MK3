// Tests für Aufstehen/Hinsetzen auf dem PC (ohne ESP32), zusammen mit Gangart und IK:
//   g++ -std=c++17 -I../src -I../include posture_test.cpp ../src/kinematics/posture.cpp ../src/kinematics/gait.cpp ../src/kinematics/kinematics.cpp -o posture_test && ./posture_test
// Läuft auch in der CI (.github/workflows/firmware.yml).

#include <cmath>
#include <cstdio>

#include "kinematics/gait.h"
#include "kinematics/kinematics.h"
#include "kinematics/posture.h"
#include "robot_geometry.h"

using namespace kin;

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failed;                                                   \
      std::printf("FEHLER %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                                    \
  do {                                                                                           \
    ++g_checks;                                                                                  \
    double _a = (a), _b = (b);                                                                   \
    if (std::fabs(_a - _b) > (tol)) {                                                            \
      ++g_failed;                                                                                \
      std::printf("FEHLER %s:%d: %s = %.4f, erwartet %.4f\n", __FILE__, __LINE__, #a, _a, _b); \
    }                                                                                            \
  } while (0)

constexpr float DT = 0.02f;

// Nachbildung von motion::update für die Haltungsübergänge: Höhe und Fußabstand
// kommen vom Sequenzer, die Gangart setzt die Füße, die IK rechnet die Winkel.
struct Sim {
  posture::Sequencer seq{posture::Config{SIT_HEIGHT, SIT_STANCE, SIT_RISE_S}};
  gait::Gait gait;
  float height = SIT_HEIGHT;
  float stance = SIT_STANCE;
  int ik_fail = 0, limit_hits = 0;
  float max_vz = 0;  // mm/s

  Sim() {
    gait::Params p;
    p.swing_s = GAIT_LIMITS.swing_default;
    p.step_height = GAIT_LIMITS.height_default;
    p.max_stride = GAIT_LIMITS.stride_max;
    gait.setParams(p);
    Vec3 n[6];
    neutral(n);
    gait.reset(n);
  }

  void neutral(Vec3 n[6]) const {
    for (int i = 0; i < 6; ++i) n[i] = stanceFoot(LEG_MOUNTS[i], stance);
  }

  void step() {
    posture::Output o = seq.update(DT, POSE_LIMITS.height_default, POSE_LIMITS.stance_default,
                                   height, gait.idle());
    float h = o.active ? o.height : POSE_LIMITS.height_default;
    float vz = std::fabs(h - height) / DT;
    if (vz > max_vz) max_vz = vz;
    height = h;
    stance = o.active ? o.stance : POSE_LIMITS.stance_default;
    Vec3 n[6];
    neutral(n);
    gait.update(DT, n);
    BodyPose pose{0, 0, height, 0, 0, 0};
    for (int i = 0; i < 6; ++i) {
      JointAngles a;
      if (!legIk(LEG_GEOMETRY, LEG_MOUNTS[i], bodyToLeg(LEG_MOUNTS[i], worldToBody(pose, gait.foot(i))), a)) {
        ++ik_fail;
        continue;
      }
      if (std::fabs(a.coxa) > 60 || a.femur < ASSEMBLY_POSE.femur - 70 || a.femur > ASSEMBLY_POSE.femur + 70 ||
          a.tibia < ASSEMBLY_POSE.tibia - 70 || a.tibia > ASSEMBLY_POSE.tibia + 70)
        ++limit_hits;
    }
  }
};

static void testSitPose() {
  // Sitzhaltung: Füße auf dem Boden erreichbar, Femur nicht am Anschlag
  BodyPose pose{0, 0, SIT_HEIGHT, 0, 0, 0};
  for (int i = 0; i < 6; ++i) {
    JointAngles a;
    Vec3 f = stanceFoot(LEG_MOUNTS[i], SIT_STANCE);
    CHECK(legIk(LEG_GEOMETRY, LEG_MOUNTS[i], bodyToLeg(LEG_MOUNTS[i], worldToBody(pose, f)), a));
    CHECK(a.femur < ASSEMBLY_POSE.femur + 60);
  }
}

static void testStandUpAndSitDown() {
  Sim s;
  s.seq.reset(posture::State::Sitting);
  for (int k = 0; k < 20; ++k) s.step();
  CHECK(s.seq.state() == posture::State::Sitting);
  CHECK(s.gait.idle());

  CHECK(s.seq.requestStand());
  CHECK(!s.seq.requestSit());  // mitten im Aufstehen nicht
  // Anheben: kein Bein hebt ab, die Füße bleiben stehen
  Vec3 planted = s.gait.foot(0);
  int k = 0;
  for (; k < SIT_RISE_S / DT - 2; ++k) {
    s.step();
    CHECK(s.gait.swingMask() == 0);
  }
  CHECK_NEAR(s.gait.foot(0).x, planted.x, 1e-3);
  // Danach Füße einsetzen und fertig
  int lifted = 0;
  for (; k < 500 && s.seq.state() != posture::State::Standing; ++k) {
    s.step();
    if (s.gait.swingMask()) ++lifted;
  }
  CHECK(s.seq.state() == posture::State::Standing);
  // Weicht der Fußabstand im Sitzen deutlich ab, werden die Füße mit Schritten
  // umgesetzt, sonst ist Aufstehen ein reines Anheben
  if (std::fabs(SIT_STANCE - POSE_LIMITS.stance_default) > 10.0f) CHECK(lifted > 0);
  else CHECK(lifted == 0);
  CHECK(k * DT < SIT_RISE_S + 2.5f);
  CHECK_NEAR(s.height, POSE_LIMITS.height_default, 1e-3);
  Vec3 n[6];
  s.neutral(n);
  for (int i = 0; i < 6; ++i) CHECK_NEAR(s.gait.foot(i).x, n[i].x, 1e-3);
  std::printf("Info: Aufstehen %.2f s, max. %.0f mm/s senkrecht\n", k * DT, s.max_vz);

  // Hinsetzen
  CHECK(s.seq.requestSit());
  for (k = 0; k < 500 && s.seq.state() != posture::State::Sitting; ++k) s.step();
  CHECK(s.seq.state() == posture::State::Sitting);
  CHECK_NEAR(s.height, SIT_HEIGHT, 1e-3);
  s.neutral(n);
  for (int i = 0; i < 6; ++i) {
    CHECK_NEAR(std::hypot(s.gait.foot(i).x - LEG_MOUNTS[i].x, s.gait.foot(i).y - LEG_MOUNTS[i].y), SIT_STANCE, 1e-2);
  }
  std::printf("Info: Hinsetzen %.2f s, max. %.0f mm/s senkrecht\n", k * DT, s.max_vz);

  CHECK(s.ik_fail == 0);
  CHECK(s.limit_hits == 0);
  CHECK(s.max_vz < 100.0f);
}
// Standhöhe/Fußabstand ändern sich mitten im Aufstehen (Regler, Profil laden):
// der Ablauf bleibt bei den Werten vom Beginn, nichts springt
static void testStandTargetChange() {
  auto run = [](bool change, float& max_dh, int& steps) {
    posture::Sequencer seq{posture::Config{SIT_HEIGHT, SIT_STANCE, SIT_RISE_S}};
    seq.reset(posture::State::Sitting);
    seq.requestStand();
    float h = SIT_HEIGHT, sh = POSE_LIMITS.height_default, ss = POSE_LIMITS.stance_default;
    float last_stance = SIT_STANCE;
    max_dh = 0;
    for (steps = 0; steps < 500; ++steps) {
      if (change && steps == (int)(0.3f * SIT_RISE_S / DT)) sh = POSE_LIMITS.height_max;
      if (change && steps == (int)(SIT_RISE_S / DT) + 2) {
        sh = POSE_LIMITS.height_min;
        ss = POSE_LIMITS.stance_max;
      }
      posture::Output o = seq.update(DT, sh, ss, h, steps > SIT_RISE_S / DT + 5);
      if (!o.active) break;
      max_dh = std::fmax(max_dh, std::fabs(o.height - h));
      // Fußabstand springt nur einmal (die Gangart setzt die Füße um), danach steht er
      if (o.stance != last_stance) CHECK(last_stance == SIT_STANCE);
      last_stance = o.stance;
      h = o.height;
    }
    CHECK(seq.state() == posture::State::Standing);
    CHECK_NEAR(h, POSE_LIMITS.height_default, 1e-3);
    CHECK_NEAR(last_stance, POSE_LIMITS.stance_default, 1e-3);
  };
  float ref_dh, dh;
  int ref_steps, n;
  run(false, ref_dh, ref_steps);
  run(true, dh, n);
  CHECK_NEAR(dh, ref_dh, 1e-4);
  CHECK(n == ref_steps);
}

int main() {
  testSitPose();
  testStandUpAndSitDown();
  testStandTargetChange();
  std::printf("%d Prüfungen, %d Fehler\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
