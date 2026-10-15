// Tests für Vorderbeine-Modus (arms), Gesten und "Lebendig" (alive) zusammen mit Gangart und IK:
//   g++ -std=c++17 -Wall -Wextra -Werror -I../src -I../include moves_test.cpp ../src/kinematics/arms.cpp ../src/kinematics/alive.cpp ../src/kinematics/gestures.cpp ../src/kinematics/gait.cpp ../src/kinematics/kinematics.cpp -o moves_test && ./moves_test

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <cstdio>
#include <vector>

#include "kinematics/alive.h"
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

// Standard-Gelenkgrenzen der Kalibrierung (±60° Coxa, ±70° Femur/Tibia um die Montagestellung)
static void defaultLimits(float lo[18], float hi[18]) {
  for (int i = 0; i < 18; ++i) {
    float ref = i % 3 == 0 ? ASSEMBLY_POSE.coxa : (i % 3 == 1 ? ASSEMBLY_POSE.femur : ASSEMBLY_POSE.tibia);
    float span = i % 3 == 0 ? 60.0f : 70.0f;
    lo[i] = ref - span;
    hi[i] = ref + span;
  }
}

static float dist(const Vec3& a, const Vec3& b) {
  return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

struct Sim {
  gait::Gait gait;
  arms::Controller arms;
  arms::Output out;
  BodyPose pose{0, 0, POSE_LIMITS.height_default, 0, 0, 0};
  float stance = POSE_LIMITS.stance_default;
  BodyPose last_pose = pose;  // Körperlage der letzten IK (wie motion.cpp)
  Vec3 feet[6];
  float lo[18], hi[18];
  float min_margin = 1e9f;
  int ik_fail = 0, limit_hits = 0, front_limit_hits = 0, below_ground = 0, knee_low = 0;
  // Vorderbeine: größter Sprung je Zyklus, kleinste Abstände (Kollision)
  float max_jump = 0, min_front_gap = 1e9f, min_mid_gap = 1e9f;

  explicit Sim(float z = POSE_LIMITS.height_default, float st = POSE_LIMITS.stance_default) {
    pose.z = z;
    last_pose = pose;
    stance = st;
    gait::Params p;
    p.swing_s = GAIT_LIMITS.swing_default;
    p.step_height = GAIT_LIMITS.height_default;
    p.max_stride = GAIT_LIMITS.stride_max;
    gait.setParams(p);
    Vec3 n[6];
    neutral(n);
    gait.reset(n);
    for (int i = 0; i < 6; ++i) feet[i] = n[i];
    defaultLimits(lo, hi);
  }
  void neutral(Vec3 n[6]) const {
    for (int i = 0; i < 6; ++i) n[i] = stanceFoot(LEG_MOUNTS[i], stance);
    n[1].x += out.mid_dx;  // Mittelbeine
    n[4].x += out.mid_dx;
  }
  static bool reach(void* ctx, int k, const Vec3& foot) {
    const Sim* s = static_cast<const Sim*>(ctx);
    const int leg = FRONT[k];
    return footReachable(LEG_GEOMETRY, LEG_MOUNTS[leg], s->last_pose, foot, &s->lo[leg * 3], &s->hi[leg * 3], 3.0f,
                         KNEE_MIN_DEG);
  }
  arms::Frame frame() {
    arms::Frame f;
    Vec3 n[6];
    neutral(n);
    for (int k = 0; k < 2; ++k) {
      f.coxa[k] = coxaWorld(last_pose, LEG_MOUNTS[FRONT[k]]);
      f.ground[k] = n[FRONT[k]];
    }
    f.reachable = reach;
    f.ctx = this;
    return f;
  }
  void step() {
    out = arms.update(DT, gait.idle(), frame());
    Vec3 n[6];
    neutral(n);
    gait.update(DT, n);
    BodyPose p = pose;
    p.x += out.body_dx;
    std::vector<Vec3> ground;
    const Vec3 prev[2] = {feet[FRONT[0]], feet[FRONT[1]]};
    for (int i = 0; i < 6; ++i) {
      feet[i] = gait.foot(i);
      bool lifted = gait.swinging(i);
      for (int k = 0; k < 2; ++k) {
        if (i != FRONT[k] || out.blend <= 0.0f) continue;
        const Vec3& a = out.foot[k];
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
      if (a.coxa < lo[i * 3] || a.coxa > hi[i * 3] || a.femur < lo[i * 3 + 1] || a.femur > hi[i * 3 + 1] ||
          a.tibia < lo[i * 3 + 2] || a.tibia > hi[i * 3 + 2]) {
        ++limit_hits;
        if (i == FRONT[0] || i == FRONT[1]) ++front_limit_hits;
      }
      if (90.0f + a.tibia < KNEE_MIN_DEG - 0.5f) ++knee_low;
    }
    for (int k = 0; k < 2; ++k) max_jump = std::max(max_jump, dist(prev[k], feet[FRONT[k]]));
    min_front_gap = std::min(min_front_gap, dist(feet[0], feet[3]));
    min_mid_gap = std::min({min_mid_gap, dist(feet[0], feet[1]), dist(feet[3], feet[4])});
    last_pose = p;
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
  // Richtung (Grad) und Abstand des Vorderfußes k zur Coxa
  void polar(int k, float& phi_deg, float& r) const {
    Vec3 c = coxaWorld(last_pose, LEG_MOUNTS[FRONT[k]]);
    const Vec3& f = feet[FRONT[k]];
    phi_deg = std::atan2(f.y - c.y, f.x - c.x) * 57.2958f;
    r = std::hypot(f.x - c.x, f.y - c.y);
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
  CHECK(s.feet[0].z > 55.0f && s.feet[3].z > 55.0f);

  // ganzer Steuerbereich: schwenken, hoch/runter, Reichweite (beide spiegelbildlich
  // und gegenläufig)
  float z_max = 0, z_min = 1e9f, phi_min = 1e9f, phi_max = -1e9f, r_min = 1e9f, r_max = 0;
  bool limited_seen = false;
  const float v[3] = {-1, 0, 1};
  for (float u : v)
    for (float z : v)
      for (float rc : v)
        for (int opp = 0; opp < 2; ++opp) {
          s.arms.setControl(u, z, opp ? -u : u, opp ? -z : z, rc);
          for (int i = 0; i < 60; ++i) {
            s.step();
            limited_seen = limited_seen || s.arms.limited();
            for (int kk = 0; kk < 2; ++kk) {
              z_max = std::max(z_max, s.feet[FRONT[kk]].z);
              z_min = std::min(z_min, s.feet[FRONT[kk]].z);
            }
            float ph, r;
            s.polar(0, ph, r);
            phi_min = std::min(phi_min, ph);
            phi_max = std::max(phi_max, ph);
            r_min = std::min(r_min, r);
            r_max = std::max(r_max, r);
          }
        }
  std::printf("Info: Vorderfuß Höhe %.0f..%.0f mm, Schwenken %.0f..%.0f°, Abstand zur Coxa %.0f..%.0f mm\n", z_min,
              z_max, phi_min, phi_max, r_min, r_max);
  CHECK(z_max > 140.0f);               // weit hoch
  CHECK(z_min > 10.0f && z_min < 25.0f);  // bis knapp über den Boden
  CHECK(phi_max - phi_min > 55.0f);    // großer Schwenkbereich
  CHECK(r_max - r_min > 50.0f);        // weit weg und heran
  CHECK(limited_seen);                 // ganz hoch und weit weg geht nicht: wird gemeldet
  s.arms.setControl(0, 0, 0, 0);
  for (int i = 0; i < 60; ++i) s.step();
  CHECK(!s.arms.limited());

  // Winken: rechter Fuß hoch, schwenkt hin und her
  CHECK(s.arms.wave());
  float wz_max = 0, wphi_min = 1e9f, wphi_max = -1e9f;
  for (int i = 0; i < 175; ++i) {
    s.step();
    wz_max = std::max(wz_max, s.feet[3].z);
    if (i > 60) {
      float ph, r;
      s.polar(1, ph, r);
      wphi_min = std::min(wphi_min, ph);
      wphi_max = std::max(wphi_max, ph);
    }
  }
  CHECK(!s.arms.waving());
  CHECK(wz_max > 110.0f);
  CHECK(wphi_max - wphi_min > 15.0f);

  // aus einer extremen Stellung absetzen (hoch, innen, weit)
  s.arms.setControl(1, 1, 1, 1, 1);
  for (int i = 0; i < 100; ++i) s.step();
  CHECK(s.arms.requestDown());
  k = s.runUntil(arms::State::Down, 600);
  CHECK(s.arms.state() == arms::State::Down);
  std::printf("Info: wieder unten nach %.1f s, kleinste Stabilitätsreserve %.0f mm, größter Fußschritt %.1f mm/Zyklus, "
              "Abstand Vorderfüße >= %.0f mm, zu den Mittelfüßen >= %.0f mm\n",
              k * DT, s.min_margin, s.max_jump, s.min_front_gap, s.min_mid_gap);
  for (int i = 0; i < 6; ++i) {
    Vec3 n = stanceFoot(LEG_MOUNTS[i], POSE_LIMITS.stance_default);
    CHECK(std::fabs(s.feet[i].x - n.x) < 1e-2f && std::fabs(s.feet[i].y - n.y) < 1e-2f && std::fabs(s.feet[i].z) < 1e-3f);
  }
  CHECK(s.ik_fail == 0);
  CHECK(s.limit_hits == 0);
  CHECK(s.knee_low == 0);
  CHECK(s.below_ground == 0);
  CHECK(s.min_margin > 15.0f);     // Schwerpunkt immer mindestens 15 mm innerhalb
  CHECK(s.max_jump < 9.0f);        // keine Sprünge (bei 220 mm/s ca. 4,4 mm je 20 ms)
  CHECK(s.min_front_gap > 50.0f);  // Vorderfüße kommen sich nicht ins Gehege
  CHECK(s.min_mid_gap > 45.0f);    // ... und den Mittelbeinen nicht
}

// Abbruch während der Gewichtsverlagerung bzw. beim Heben: geht direkt zurück
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
  // mitten im Heben absetzen: ohne Umweg wieder runter
  s.arms.requestUp();
  s.runUntil(arms::State::Raise, 400);
  for (int i = 0; i < 25; ++i) s.step();
  float z = s.feet[0].z;
  CHECK(z > 5.0f && z < 55.0f);
  CHECK(s.arms.requestDown());
  float z_peak = 0;
  int k = 0;
  while (s.arms.state() == arms::State::Lower && k++ < 200) {
    s.step();
    z_peak = std::max(z_peak, s.feet[0].z);
  }
  CHECK(z_peak <= z + 1e-3f);
  s.runUntil(arms::State::Down, 400);
  CHECK(s.arms.state() == arms::State::Down);
  CHECK(s.ik_fail == 0 && s.below_ground == 0 && s.max_jump < 9.0f);
  // reset: sofort unten
  s.arms.requestUp();
  s.runUntil(arms::State::Up, 400);
  s.arms.reset();
  CHECK(s.arms.state() == arms::State::Down);
  CHECK(s.arms.update(DT, true, s.frame()).blend == 0.0f);
}

// Andere Körperhaltungen: überall, wo die Gewichtsverlagerung erreichbar ist (prüft
// motion::setArms vorher), bleiben die Vorderbeine erreichbar und in den Grenzen.
// (Bei sehr schmalem Stand stößt der Umsetzschritt der Mittelbeine kurz an die
// Femur-Grenze – wie beim Laufen; das Servo begrenzt dann den Fußhub.)
static void testPoses() {
  int tried = 0;
  for (float z : {35.0f, 45.0f, 52.0f, 70.0f, 85.0f, 100.0f}) {
    for (float st : {75.0f, 90.0f, 100.0f, 115.0f, 135.0f}) {
      Sim s(z, st);
      const arms::Config ac;
      BodyPose shifted = s.pose;
      shifted.x -= ac.cog_shift;
      Vec3 n[6];
      for (int i = 0; i < 6; ++i) n[i] = stanceFoot(LEG_MOUNTS[i], st);
      n[1].x += ac.mid_forward;
      n[4].x += ac.mid_forward;
      if (!checkPose(LEG_GEOMETRY, LEG_MOUNTS, 6, shifted, n, s.lo, s.hi).ok()) continue;
      ++tried;
      s.arms.requestUp();
      s.runUntil(arms::State::Up, 400);
      const float c[][5] = {{1, 1, 1, 1, 1}, {-1, -1, -1, -1, -1}, {1, -1, -1, 1, 0}, {-1, 1, 1, -1, 1}};
      for (const auto& v : c) {
        s.arms.setControl(v[0], v[1], v[2], v[3], v[4]);
        for (int i = 0; i < 60; ++i) s.step();
      }
      s.arms.requestDown();
      s.runUntil(arms::State::Down, 600);
      CHECK(s.arms.state() == arms::State::Down);
      CHECK(s.ik_fail == 0);
      CHECK(s.front_limit_hits == 0);
      CHECK(s.below_ground == 0);
      CHECK(s.min_margin > 15.0f);
      CHECK(s.max_jump < 9.0f);
      if (s.ik_fail || s.front_limit_hits || s.max_jump >= 9.0f)
        std::printf("  bei Höhe %.0f, Fußabstand %.0f: IK %d, Grenzen %d, Sprung %.1f\n", z, st, s.ik_fail,
                    s.front_limit_hits, s.max_jump);
      if (s.limit_hits > s.front_limit_hits)
        std::printf("Info: Höhe %.0f, Fußabstand %.0f: Mittelbein beim Umsetzen %d Zyklen an der Grenze\n", z, st,
                    s.limit_hits - s.front_limit_hits);
    }
  }
  std::printf("Info: Vorderbeine in %d Haltungen geprüft\n", tried);
  CHECK(tried >= 10);
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
  defaultLimits(lo, hi);
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

// "Lebendig": lange laufen lassen – weich, immer nur ein Bein in der Luft, stabil,
// erreichbar; Abschalten bzw. Laufen blendet zügig aus
// Gähnen, Seufzen, Strecken (Langeweile/Aufwachen): erreichbar, standsicher, ruhig
static void testMoodActions() {
  float lo[18], hi[18];
  defaultLimits(lo, hi);
  Vec3 n[6];
  for (int i = 0; i < 6; ++i) n[i] = stanceFoot(LEG_MOUNTS[i], POSE_LIMITS.stance_default);
  for (alive::Action act : {alive::Action::Yawn, alive::Action::Sigh, alive::Action::Stretch}) {
    alive::Animator a(LEG_MOUNTS, 77);
    a.setBreath(false);
    a.play(act);
    alive::Output prev;
    int bad = 0, two_up = 0;
    float min_margin = 1e9f, body_jump = 0, lift_max = 0;
    for (int i = 0; i < 1000 && a.playing(); ++i) {
      alive::Output o = a.update(DT, false, true);
      body_jump = std::max(body_jump, jumpOf(o.body, prev.body));
      prev = o;
      BodyPose p{o.body.x, o.body.y, POSE_LIMITS.height_default + o.body.z, o.body.roll, o.body.pitch, o.body.yaw};
      Vec3 f[6];
      std::vector<Vec3> ground;
      int up = 0;
      for (int leg = 0; leg < 6; ++leg) {
        f[leg] = {n[leg].x + o.foot[leg].x, n[leg].y + o.foot[leg].y, n[leg].z + o.foot[leg].z};
        if (o.foot[leg].z > 0.5f) ++up;
        else ground.push_back(f[leg]);
        lift_max = std::max(lift_max, o.foot[leg].z);
      }
      if (up > 1) ++two_up;
      if (!checkPose(LEG_GEOMETRY, LEG_MOUNTS, 6, p, f, lo, hi).ok()) ++bad;
      min_margin = std::min(min_margin, stabilityMargin(ground, p.x, p.y));
    }
    std::printf("Info: %s: Stabilitätsreserve >= %.0f mm, Sprung je Zyklus %.2f, Bein bis %.0f mm\n",
                alive::actionName(act), min_margin, body_jump, lift_max);
    CHECK(!a.playing());
    CHECK(bad == 0);
    CHECK(two_up == 0);
    CHECK(min_margin > 25.0f);
    CHECK(body_jump < 1.5f);
    if (act == alive::Action::Stretch) CHECK(lift_max > 25.0f);
  }
}

static void testAlive() {
  float lo[18], hi[18];
  defaultLimits(lo, hi);
  Vec3 n[6];
  for (int i = 0; i < 6; ++i) n[i] = stanceFoot(LEG_MOUNTS[i], POSE_LIMITS.stance_default);

  alive::Animator a(LEG_MOUNTS, 12345);
  alive::Output prev;
  int seen[alive::NUM_ACTIONS] = {}, starts = 0, bad = 0, two_up = 0, below = 0;
  float body_jump = 0, foot_jump = 0, min_margin = 1e9f, yaw_max = 0, lift_max = 0, shift_max = 0;
  alive::Action last = alive::Action::None;
  const int STEPS = static_cast<int>(600.0f / DT);
  for (int i = 0; i < STEPS; ++i) {
    alive::Output o = a.update(DT, true, true);
    if (a.action() != last && a.action() != alive::Action::None) {
      ++seen[static_cast<int>(a.action())];
      ++starts;
    }
    last = a.action();
    body_jump = std::max(body_jump, jumpOf(o.body, prev.body));
    BodyPose p{o.body.x, o.body.y, POSE_LIMITS.height_default + o.body.z, o.body.roll, o.body.pitch, o.body.yaw};
    Vec3 f[6];
    std::vector<Vec3> ground;
    int up = 0;
    for (int leg = 0; leg < 6; ++leg) {
      foot_jump = std::max(foot_jump, dist(o.foot[leg], prev.foot[leg]));
      f[leg] = {n[leg].x + o.foot[leg].x, n[leg].y + o.foot[leg].y, n[leg].z + o.foot[leg].z};
      if (o.foot[leg].z < -1e-4f) ++below;
      if (o.foot[leg].z > 0.5f) ++up;
      else ground.push_back(f[leg]);
      lift_max = std::max(lift_max, o.foot[leg].z);
    }
    if (up > 1) ++two_up;
    if (!checkPose(LEG_GEOMETRY, LEG_MOUNTS, 6, p, f, lo, hi).ok()) ++bad;
    min_margin = std::min(min_margin, stabilityMargin(ground, p.x, p.y));
    yaw_max = std::max(yaw_max, std::fabs(o.body.yaw));
    shift_max = std::max(shift_max, std::hypot(o.body.x, o.body.y));
    prev = o;
  }
  std::printf("Info: Lebendig in 10 min: %d Aktionen (Umschauen %d, Gewicht %d, Kopf schief %d, Bein heben %d, "
              "Tippen %d, Neu setzen %d), Stabilitätsreserve >= %.0f mm, Sprung je Zyklus Körper %.2f, Fuß %.2f mm\n",
              starts, seen[1], seen[2], seen[3], seen[4], seen[5], seen[6], min_margin, body_jump, foot_jump);
  // zufällig: alle bis "Neu setzen" (Gähnen, Seufzen, Strecken nur über die Stimmung)
  for (int i = 1; i <= static_cast<int>(alive::Action::Shuffle); ++i) CHECK(seen[i] > 0);
  for (int i = static_cast<int>(alive::Action::Yawn); i < alive::NUM_ACTIONS; ++i) CHECK(seen[i] == 0);
  CHECK(starts > 50 && starts < 120);  // etwa alle 5–12 s
  CHECK(bad == 0);
  CHECK(two_up == 0);
  CHECK(below == 0);
  CHECK(min_margin > 40.0f);
  CHECK(body_jump < 1.0f);   // je 20 ms höchstens 1 mm bzw. 1° (in Summe): ruhig
  CHECK(foot_jump < 4.0f);   // Tippen ist flott (ca. 180 mm/s)
  CHECK(yaw_max > 7.0f);     // sichtbar
  CHECK(lift_max > 12.0f);
  CHECK(shift_max > 5.0f && shift_max < 15.0f);

  // Laufen (nicht mehr erlaubt) mitten im Beinheben: blendet zügig und ohne Sprung aus
  a.start(alive::Action::Lift);
  float up_max = 0;
  for (int i = 0; i < 40; ++i) {
    prev = a.update(DT, true, true);
    for (const Vec3& fo : prev.foot) up_max = std::max(up_max, fo.z);
  }
  CHECK(up_max > 10.0f);
  float jump = 0;
  int k = 0;
  alive::Output o;
  for (; k < 50; ++k) {
    o = a.update(DT, true, false);
    jump = std::max(jump, jumpOf(o.body, prev.body));
    for (int leg = 0; leg < 6; ++leg) jump = std::max(jump, dist(o.foot[leg], prev.foot[leg]));
    prev = o;
    if (a.silent()) break;
  }
  CHECK(a.silent());
  CHECK(k * DT < 0.45f);
  CHECK(jump < 4.0f);
  CHECK(jumpOf(o.body, gestures::Offset()) < 1e-4f);
  // danach erst nach einer Pause wieder
  int quiet = 0;
  while (a.update(DT, true, true), a.action() == alive::Action::None && quiet < 1000) ++quiet;
  CHECK(quiet * DT > 2.5f);

  // ausgeschaltet: nichts
  alive::Animator off(LEG_MOUNTS, 7);
  float any = 0;
  for (int i = 0; i < 1000; ++i) {
    o = off.update(DT, false, true);
    any = std::max(any, std::fabs(o.body.z) + std::fabs(o.body.yaw) + std::fabs(o.foot[0].z));
  }
  CHECK(any == 0.0f);
  CHECK(off.action() == alive::Action::None);
  // eingeschaltet: bald die erste Aktion (Rückmeldung für den Nutzer)
  k = 0;
  while (off.update(DT, true, true), off.action() == alive::Action::None && k < 1000) ++k;
  CHECK(k * DT < 3.0f);
}

int main() {
  testFullSequence();
  testAbort();
  testPoses();
  testGestures();
  testAlive();
  testMoodActions();
  std::printf("%d Prüfungen, %d Fehler\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
