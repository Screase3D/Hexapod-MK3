// Tests der Gangarten auf dem PC (ohne ESP32):
//   g++ -std=c++17 -I../src -I../include gait_test.cpp ../src/kinematics/gait.cpp ../src/kinematics/kinematics.cpp -o gait_test && ./gait_test
// Läuft auch in der CI (.github/workflows/firmware.yml).

#include <cmath>
#include <cstdio>

#include "kinematics/gait.h"
#include "kinematics/kinematics.h"
#include "config.h"
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

constexpr float DT = 0.02f;  // 50 Hz wie der Regel-Task
const gait::Type TYPES[] = {gait::Type::Tripod, gait::Type::Ripple, gait::Type::Wave};
const int MAX_SWING[] = {3, 2, 1};

struct Cmd {
  float vx, vy, turn;
  const char* name;
};
const Cmd CMDS[] = {
    {1, 0, 0, "vor"},      {-1, 0, 0, "zurück"},  {0, 1, 0, "links"},
    {0, 0, 1, "drehen"},   {0, 0, -1, "drehen rechts"}, {0.7f, -0.7f, 0, "schräg"},
    {1, 0, 1, "Kurve"},    {-0.5f, 0.5f, -0.8f, "gemischt"},
};

static void neutralFeet(Vec3 out[6], float stance) {
  for (int i = 0; i < 6; ++i) out[i] = stanceFoot(LEG_MOUNTS[i], stance);
}

// Standardwerte der Firmware (robot_geometry.h)
static gait::Params firmwareParams(gait::Type t) {
  gait::Params p;
  p.type = t;
  p.swing_s = GAIT_LIMITS.swing_default;
  p.step_height = GAIT_LIMITS.height_default;
  p.max_stride = GAIT_LIMITS.stride_max;
  return p;
}

// Reichweite wie in motion.cpp (Körper in der Standardhaltung)
static void setReach(gait::Gait& g, float step_height) {
  const float h = POSE_LIMITS.height_default;
  BodyPose pose{0, 0, h, 0, 0, 0};
  Vec3 coxa[6];
  for (int i = 0; i < 6; ++i) coxa[i] = coxaWorld(pose, LEG_MOUNTS[i]);
  g.setReach(coxa, gait::minFootRadius(LEG_GEOMETRY, POSE_LIMITS.stance_default, h, step_height, KNEE_MIN_DEG),
             gait::maxFootRadius(LEG_GEOMETRY, h));
}

static gait::Gait makeGait(gait::Type t, const Vec3 neutral[6], const gait::Params* params = nullptr) {
  gait::Gait g;
  g.setParams(params ? *params : firmwareParams(t));
  setReach(g, g.params().step_height);
  g.reset(neutral);
  return g;
}

static void testProfiles() {
  CHECK_NEAR(gait::smoothStep5(0), 0, 1e-6);
  CHECK_NEAR(gait::smoothStep5(1), 1, 1e-6);
  CHECK_NEAR(gait::smoothStep5(0.5f), 0.5, 1e-6);
  CHECK_NEAR(gait::liftProfile(0), 0, 1e-6);
  CHECK_NEAR(gait::liftProfile(0.5f), 1, 1e-6);
  CHECK_NEAR(gait::liftProfile(1), 0, 1e-6);
  float prev = 0;
  for (int k = 1; k <= 100; ++k) {
    float v = gait::smoothStep5(k / 100.0f);
    CHECK(v >= prev);
    prev = v;
  }
  gait::Type t;
  CHECK(gait::typeFromName("ripple", t) && t == gait::Type::Ripple);
  CHECK(!gait::typeFromName("hüpfen", t));
}

// Ohne Befehl: Stillstand, kein Bein hebt ab, Füße folgen dem Fußabstand
static void testIdle() {
  Vec3 n[6];
  neutralFeet(n, POSE_LIMITS.stance_default);
  gait::Gait g = makeGait(gait::Type::Tripod, n);
  for (int k = 0; k < 200; ++k) g.update(DT, n);
  CHECK(g.idle());
  CHECK(g.swingMask() == 0);
  // kleine Änderung (Regler): Füße folgen direkt
  Vec3 wide[6];
  neutralFeet(wide, POSE_LIMITS.stance_default + 5);
  g.update(DT, wide);
  CHECK_NEAR(g.foot(0).x, wide[0].x, 1e-4);
  CHECK(g.idle());
  // großer Sprung (Aufstehen): Füße werden mit Schritten umgesetzt
  neutralFeet(wide, POSE_LIMITS.stance_default + 30);
  g.update(DT, wide);
  CHECK(!g.idle());
  CHECK(g.swingMask() != 0);
  int k = 0;
  while (!g.idle() && k < 200) {
    g.update(DT, wide);
    ++k;
  }
  CHECK(g.idle());
  CHECK(k * DT < 1.5f * gait::cycleTime(gait::Type::Tripod, GAIT_LIMITS.swing_default));
  for (int i = 0; i < 6; ++i) CHECK_NEAR(g.foot(i).y, wide[i].y, 1e-3);
}

// Standbeine dürfen auf dem Boden nicht rutschen; gleichzeitig angehobene
// Beine je Gangart begrenzt; keine Sprünge; alle Fußpunkte per IK erreichbar
static void testWalking(bool extreme) {
  Vec3 n[6];
  neutralFeet(n, POSE_LIMITS.stance_default);
  BodyPose pose{0, 0, POSE_LIMITS.height_default, 0, 0, 0};
  int ik_fail = 0, limit_hits = 0;
  float max_jump = 0, max_slip = 0, max_rate_swing = 0, max_rate_stance = 0;
  float knee_min = 1e9f, speed_fwd = 0;
  float jmin[3] = {1e9f, 1e9f, 1e9f}, jmax[3] = {-1e9f, -1e9f, -1e9f};

  for (int ti = 0; ti < 3; ++ti) {
    for (const Cmd& c : CMDS) {
      // extrem: höchster Fußhub und kürzeste Schrittdauer der Oberfläche
      gait::Params p = firmwareParams(TYPES[ti]);
      if (extreme) {
        p.step_height = GAIT_LIMITS.height_max;
        p.swing_s = GAIT_LIMITS.swing_min;
      }
      gait::Gait g = makeGait(TYPES[ti], n, &p);
      g.setCommand(c.vx, c.vy, c.turn);
      // Körper im Weltsystem mitführen, um das Rutschen der Standbeine zu messen
      float bx = 0, by = 0, byaw = 0;
      Vec3 planted[6];
      bool was_stance[6] = {};
      Vec3 prev[6];
      for (int i = 0; i < 6; ++i) prev[i] = g.foot(i);
      float prev_ang[6][3];
      bool have_prev = false;
      for (int k = 0; k < 400; ++k) {  // 8 s
        g.update(DT, n);
        // Körperbewegung dieses Schritts (wie im Gang: erst verschieben, dann drehen)
        float cy = std::cos(byaw), sy = std::sin(byaw);
        bx += (cy * g.speedX() - sy * g.speedY()) * DT;
        by += (sy * g.speedX() + cy * g.speedY()) * DT;
        byaw += g.turnRate() * DT;
        cy = std::cos(byaw);
        sy = std::sin(byaw);

        int swinging = 0;
        for (int i = 0; i < 6; ++i) {
          const Vec3& f = g.foot(i);
          if (g.swinging(i)) ++swinging;
          float jump = std::hypot(std::hypot(f.x - prev[i].x, f.y - prev[i].y), f.z - prev[i].z);
          if (jump > max_jump) max_jump = jump;
          prev[i] = f;

          Vec3 w = {bx + cy * f.x - sy * f.y, by + sy * f.x + cy * f.y, f.z};
          bool stance = !g.swinging(i);
          if (stance && was_stance[i]) {
            float slip = std::hypot(w.x - planted[i].x, w.y - planted[i].y);
            if (slip > max_slip) max_slip = slip;
          }
          if (stance && !was_stance[i]) planted[i] = w;
          was_stance[i] = stance;
          if (stance) CHECK_NEAR(f.z, 0, 1e-4);

          JointAngles a;
          Vec3 leg = bodyToLeg(LEG_MOUNTS[i], worldToBody(pose, f));
          if (!legIk(LEG_GEOMETRY, LEG_MOUNTS[i], leg, a)) {
            ++ik_fail;
            continue;
          }
          if (90.0f + a.tibia < knee_min) knee_min = 90.0f + a.tibia;
          float v[3] = {a.coxa, a.femur, a.tibia};
          for (int j = 0; j < 3; ++j) {
            if (v[j] < jmin[j]) jmin[j] = v[j];
            if (v[j] > jmax[j]) jmax[j] = v[j];
            if (have_prev) {
              float rate = std::fabs(v[j] - prev_ang[i][j]) / DT;
              float& m = g.swinging(i) ? max_rate_swing : max_rate_stance;
              if (rate > m) m = rate;
            }
            prev_ang[i][j] = v[j];
          }
          // Standardgrenzen der Kalibrierung (Montagestellung ± 60/70°)
          if (std::fabs(a.coxa) > 60 || a.femur < ASSEMBLY_POSE.femur - 70 ||
              a.femur > ASSEMBLY_POSE.femur + 70 || a.tibia < ASSEMBLY_POSE.tibia - 70 ||
              a.tibia > ASSEMBLY_POSE.tibia + 70)
            ++limit_hits;
        }
        CHECK(swinging <= MAX_SWING[ti]);
        have_prev = true;
      }
      CHECK(!g.idle());
      // Ziel-Geschwindigkeit wird erreicht (nach der Rampe)
      if (c.turn == 0 && c.vx == 1) CHECK_NEAR(g.speedX(), g.maxSpeed(), 1e-3);
      if (c.turn == 0 && c.vx == 1 && ti == 0) speed_fwd = g.speedX();
    }
  }
  std::printf("Info (%s): Gang max. Sprung %.2f mm/Schritt, Rutschen %.3f mm, Gelenk max. %.0f °/s "
              "(Stand) / %.0f °/s (Schwung), coxa %.0f..%.0f, femur %.0f..%.0f, tibia %.0f..%.0f, "
              "Knie min. %.1f°, Dreifuß vorwärts %.0f mm/s\n",
              extreme ? "Extremwerte" : "Standard", max_jump, max_slip, max_rate_stance, max_rate_swing,
              jmin[0], jmax[0], jmin[1], jmax[1], jmin[2], jmax[2], knee_min, speed_fwd);
  // Kniewinkel: die Reichweitengrenze rechnet mit einem Schritt genau zum Körper
  // hin; beim Anlaufen in Kurven (Welle, 25 mm Hub) geht es kurz etwas darunter
  CHECK(knee_min > KNEE_MIN_DEG - (extreme ? 5.0f : 1.0f));
  CHECK(ik_fail == 0);
  CHECK(limit_hits == 0);
  CHECK(max_slip < 0.05f);
  if (!extreme) {
    CHECK(max_jump < 8.0f);  // 400 mm/s bei 50 Hz
    // Standbeine (tragen das Gewicht) bleiben unter der normalen Servo-Rampe,
    // Schwungbeine (unbelastet) unter der Rampe im IK-Betrieb
    CHECK(max_rate_stance < SERVO_SLEW_DEG_PER_S);
    CHECK(max_rate_swing < GAIT_SERVO_SLEW_DEG_PER_S);
  }
}

// Loslassen: Beine laufen in die Neutralstellung und der Gang hält an
static void testStop() {
  Vec3 n[6];
  neutralFeet(n, POSE_LIMITS.stance_default);
  for (gait::Type t : TYPES) {
    gait::Gait g = makeGait(t, n);
    g.setCommand(1, 0, 0.5f);
    for (int k = 0; k < 150; ++k) g.update(DT, n);
    g.setCommand(0, 0, 0);
    int steps = 0;
    while (!g.idle() && steps < 500) {
      g.update(DT, n);
      ++steps;
    }
    CHECK(g.idle());
    // höchstens Rampe + gut ein Zyklus
    CHECK(steps * DT < g.params().ramp_s + 1.5f * gait::cycleTime(t, g.params().swing_s));
    for (int i = 0; i < 6; ++i) {
      CHECK_NEAR(g.foot(i).x, n[i].x, 1e-3);
      CHECK_NEAR(g.foot(i).z, 0, 1e-3);
    }
    // im Stillstand hebt nichts mehr ab
    for (int k = 0; k < 100; ++k) {
      g.update(DT, n);
      CHECK(g.swingMask() == 0);
    }
  }
}

// Anfahren: die erste Beingruppe hebt sofort ab; Gangart-Wechsel erst im Stillstand
static void testStartAndSwitch() {
  Vec3 n[6];
  neutralFeet(n, POSE_LIMITS.stance_default);
  gait::Gait g = makeGait(gait::Type::Tripod, n);
  g.setCommand(1, 0, 0);
  g.update(DT, n);
  g.update(DT, n);
  CHECK(g.swingMask() == 0b010101);  // vl, hl, mr

  gait::Params p = g.params();
  p.type = gait::Type::Wave;
  g.setParams(p);
  for (int k = 0; k < 20; ++k) g.update(DT, n);
  CHECK(g.activeType() == gait::Type::Tripod);  // läuft noch
  g.setCommand(0, 0, 0);
  for (int k = 0; k < 300 && !g.idle(); ++k) g.update(DT, n);
  CHECK(g.idle());
  CHECK(g.activeType() == gait::Type::Wave);
}

// Anlaufen aus dem Stillstand: Das Bein mit dem spätesten Schwungfenster wandert
// bis zu seinem ersten Schritt mit; die Anlauf-Rampe hält seine Auslenkung nahe an
// der des eingeschwungenen Laufs (halbe Schrittlänge). Ohne sie wäre es beim
// Wellengang fast das Doppelte -> Knie knickt weit ein.
static void testStartExcursion() {
  Vec3 n[6];
  neutralFeet(n, POSE_LIMITS.stance_default);
  for (gait::Type t : TYPES) {
    gait::Gait g = makeGait(t, n);
    g.setCommand(1, 0, 0);
    float worst = 0;
    const int steps = static_cast<int>(3 * gait::cycleTime(t, g.params().swing_s) / DT);
    for (int k = 0; k < steps; ++k) {
      g.update(DT, n);
      for (int i = 0; i < 6; ++i) {
        if (!g.swinging(i)) worst = std::fmax(worst, std::fabs(g.foot(i).x - n[i].x));
      }
    }
    CHECK(worst < 1.3f * 0.5f * g.params().max_stride);
  }
}

int main() {
  testProfiles();
  testIdle();
  testWalking(false);
  testWalking(true);
  testStop();
  testStartAndSwitch();
  testStartExcursion();
  std::printf("%d Prüfungen, %d Fehler\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
