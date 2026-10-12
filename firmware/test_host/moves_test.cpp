// Tests für Vorderbeine-Modus (arms) und Gesten zusammen mit Gangart und IK:
//   g++ -std=c++17 -Wall -Wextra -Werror -I../src -I../include moves_test.cpp ../src/kinematics/arms.cpp ../src/kinematics/gestures.cpp ../src/kinematics/gait.cpp ../src/kinematics/kinematics.cpp -o moves_test && ./moves_test

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "kinematics/arms.h"
#include "kinematics/gait.h"
#include "kinematics/gestures.h"
#include "robot_geometry.h"

using namespace kin;

static int g_checks = 0, g_failed = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failed;                                                   \
      std::printf("FEHLER %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

constexpr float DT = 0.02f;
constexpr int FRONT[2] = {0, 3};  // Bein 1 (vorne links), Bein 4 (vorne rechts)

// Abstand des Punkts p zum Rand der konvexen Hülle der Punkte pts
// (positiv = innen, negativ = außen)
static float stabilityMargin(std::vector<Vec3> pts, float px, float py) {
  if (pts.size() < 3) return -1e9f;
  std::sort(pts.begin(), pts.end(), [](const Vec3& a, const Vec3& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
  auto cross = [](const Vec3& o, const Vec3& a, const Vec3& b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
  };
  std::vector<Vec3> h(2 * pts.size());
  size_t k = 0;
  for (size_t i = 0; i < pts.size(); ++i) {
    while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
    h[k++] = pts[i];
  }
  for (size_t i = pts.size() - 1, t = k + 1; i-- > 0;) {
    while (k >= t && cross(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
    h[k++] = pts[i];
  }
  h.resize(k - 1);  // gegen den Uhrzeigersinn
  float margin = 1e9f;
  for (size_t i = 0; i < h.size(); ++i) {
    const Vec3& a = h[i];
    const Vec3& b = h[(i + 1) % h.size()];
    float ex = b.x - a.x, ey = b.y - a.y, len = std::hypot(ex, ey);
    float d = (ex * (py - a.y) - ey * (px - a.x)) / len;  // > 0 links der Kante = innen
    margin = std::min(margin, d);
  }
  return margin;
}

struct Sim {
  gait::Gait gait;
  arms::Controller arms;
  arms::Output out;
  BodyPose pose{0, 0, POSE_LIMITS.height_default, 0, 0, 0};
  Vec3 feet[6];
  float min_margin = 1e9f;
  int ik_fail = 0, limit_hits = 0, below_ground = 0;

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
    for (int i = 0; i < 6; ++i) n[i] = stanceFoot(LEG_MOUNTS[i], POSE_LIMITS.stance_default);
    n[1].x += out.mid_dx;  // Mittelbeine
    n[4].x += out.mid_dx;
  }
  void step() {
    out = arms.update(DT, gait.idle());
    Vec3 n[6];
    neutral(n);
    gait.update(DT, n);
    BodyPose p = pose;
    p.x += out.body_dx;
    std::vector<Vec3> ground;
    for (int i = 0; i < 6; ++i) {
      feet[i] = gait.foot(i);
      bool lifted = gait.swinging(i);
      for (int k = 0; k < 2; ++k) {
        if (i != FRONT[k] || out.blend <= 0.0f) continue;
        Vec3 a = {n[i].x + out.foot[k].x, n[i].y + out.foot[k].y, out.foot[k].z};
        feet[i] = {feet[i].x + (a.x - feet[i].x) * out.blend, feet[i].y + (a.y - feet[i].y) * out.blend,
                   feet[i].z + (a.z - feet[i].z) * out.blend};
        if (feet[i].z > 0.5f) lifted = true;
      }
      if (feet[i].z < -1e-3f) ++below_ground;
      if (!lifted) ground.push_back(feet[i]);
      JointAngles a;
      if (!legIk(LEG_GEOMETRY, LEG_MOUNTS[i], bodyToLeg(LEG_MOUNTS[i], worldToBody(p, feet[i])), a)) {
        ++ik_fail;
        continue;
      }
      if (std::fabs(a.coxa) > 60 || std::fabs(a.femur - ASSEMBLY_POSE.femur) > 70 ||
          std::fabs(a.tibia - ASSEMBLY_POSE.tibia) > 70)
        ++limit_hits;
    }
    // Schwerpunkt ~ Körpermitte (Beine leicht gegenüber dem Körper mit Elektronik/Akku)
    min_margin = std::min(min_margin, stabilityMargin(ground, p.x, p.y));
  }
  int runUntil(arms::State s, int max_steps) {
    int k = 0;
    while (arms.state() != s && k < max_steps) {
      step();
      ++k;
    }
    return k;
  }
};

static void testFullSequence() {
  Sim s;
  CHECK(s.arms.requestUp());
  int k = s.runUntil(arms::State::Up, 400);
  CHECK(s.arms.state() == arms::State::Up);
  std::printf("Info: Vorderbeine oben nach %.1f s\n", k * DT);
  // Bereitstellung: beide Vorderfüße deutlich über dem Boden
  for (int i = 0; i < 30; ++i) s.step();
  CHECK(s.feet[0].z > 40.0f && s.feet[3].z > 40.0f);
  // gesamter Steuerbereich und Winken
  const float corners[][4] = {{1, 1, -1, -1}, {-1, -1, 1, 1}, {1, -1, -1, 1}, {0, 0, 0, 0}};
  for (const auto& c : corners) {
    s.arms.setControl(c[0], c[1], c[2], c[3]);
    for (int i = 0; i < 60; ++i) s.step();
  }
  CHECK(s.arms.wave());
  float z_max = 0;
  for (int i = 0; i < 175; ++i) {
    s.step();
    z_max = std::max(z_max, s.feet[3].z);
  }
  CHECK(!s.arms.waving());
  CHECK(z_max > 70.0f);  // winkt mit erhobenem Fuß
  // absetzen
  CHECK(s.arms.requestDown());
  k = s.runUntil(arms::State::Down, 400);
  CHECK(s.arms.state() == arms::State::Down);
  std::printf("Info: wieder unten nach %.1f s, kleinste Stabilitätsreserve %.0f mm\n", k * DT, s.min_margin);
  for (int i = 0; i < 6; ++i) {
    Vec3 n = stanceFoot(LEG_MOUNTS[i], POSE_LIMITS.stance_default);
    CHECK(std::fabs(s.feet[i].x - n.x) < 1e-2f && std::fabs(s.feet[i].y - n.y) < 1e-2f && std::fabs(s.feet[i].z) < 1e-3f);
  }
  CHECK(s.ik_fail == 0);
  CHECK(s.limit_hits == 0);
  CHECK(s.below_ground == 0);
  CHECK(s.min_margin > 15.0f);  // Schwerpunkt immer mindestens 15 mm innerhalb
}

// Abbruch während der Gewichtsverlagerung: geht direkt zurück
static void testAbort() {
  Sim s;
  s.arms.requestUp();
  for (int i = 0; i < 20; ++i) s.step();
  CHECK(s.arms.state() == arms::State::ShiftBack);
  CHECK(s.arms.requestDown());
  CHECK(s.arms.state() == arms::State::ShiftForward);
  s.runUntil(arms::State::Down, 400);
  CHECK(s.arms.state() == arms::State::Down);
  CHECK(std::fabs(s.out.body_dx) < 1e-6f);
  CHECK(s.min_margin > 15.0f);
  CHECK(!s.arms.wave());  // nur oben
  // reset: sofort unten
  s.arms.requestUp();
  s.runUntil(arms::State::Up, 400);
  s.arms.reset();
  CHECK(s.arms.state() == arms::State::Down);
  CHECK(s.arms.update(DT, true).blend == 0.0f);
}

// Gesten: weich ein/aus, Abbruch ohne Sprung, alle Stellungen erreichbar
static float jumpOf(const gestures::Offset& a, const gestures::Offset& b) {
  float d = std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z);
  return std::max(d, std::fabs(a.roll - b.roll) + std::fabs(a.pitch - b.pitch) + std::fabs(a.yaw - b.yaw));
}

static void testGestures() {
  const gestures::Type types[] = {gestures::Type::Circle, gestures::Type::Nod, gestures::Type::Shake,
                                  gestures::Type::Pushup, gestures::Type::Dance};
  Vec3 feet[6];
  for (int i = 0; i < 6; ++i) feet[i] = stanceFoot(LEG_MOUNTS[i], POSE_LIMITS.stance_default);
  float lo[18], hi[18];
  for (int i = 0; i < 18; ++i) {
    float ref = i % 3 == 0 ? ASSEMBLY_POSE.coxa : (i % 3 == 1 ? ASSEMBLY_POSE.femur : ASSEMBLY_POSE.tibia);
    float span = i % 3 == 0 ? 60.0f : 70.0f;
    lo[i] = ref - span;
    hi[i] = ref + span;
  }
  for (gestures::Type t : types) {
    gestures::Player g;
    g.start(t);
    const float d = g.duration();
    gestures::Offset prev, o;
    float max_jump = 0, amp = 0;
    int steps = 0, bad = 0;
    while (g.active() != gestures::Type::None && steps < 1000) {
      o = g.update(DT);
      max_jump = std::max(max_jump, jumpOf(o, prev));
      amp = std::max(amp, std::fabs(o.x) + std::fabs(o.y) + std::fabs(o.z) + std::fabs(o.roll) +
                              std::fabs(o.pitch) + std::fabs(o.yaw));
      prev = o;
      BodyPose p{o.x, o.y, POSE_LIMITS.height_default + o.z, o.roll, o.pitch, o.yaw};
      if (!checkPose(LEG_GEOMETRY, LEG_MOUNTS, 6, p, feet, lo, hi).ok()) ++bad;
      ++steps;
    }
    CHECK(g.active() == gestures::Type::None);
    CHECK(std::fabs(steps * DT - d) < 0.05f);
    CHECK(jumpOf(o, gestures::Offset()) < 0.5f);  // endet bei null
    CHECK(max_jump < 2.0f);                         // je 20 ms höchstens 2 mm bzw. 2° (100 °/s)
    CHECK(amp > 5.0f);                              // bewegt sich wirklich
    CHECK(bad == 0);
    // Abbruch in der Mitte: ohne Sprung, nach dem Ausblenden vorbei
    g.start(t);
    for (int k = 0; k < static_cast<int>(d / DT / 2); ++k) prev = g.update(DT);
    g.stop();
    int k = 0;
    float jump = 0;
    while (g.active() != gestures::Type::None && k < 200) {
      o = g.update(DT);
      jump = std::max(jump, jumpOf(o, prev));
      prev = o;
      ++k;
    }
    CHECK(jump < 2.0f);
    CHECK(k * DT < 0.85f);
  }
  gestures::Type t;
  CHECK(gestures::typeFromName("pushup", t) && t == gestures::Type::Pushup);
  CHECK(!gestures::typeFromName("none", t));
}

int main() {
  testFullSequence();
  testAbort();
  testGestures();
  std::printf("%d Prüfungen, %d Fehler\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
