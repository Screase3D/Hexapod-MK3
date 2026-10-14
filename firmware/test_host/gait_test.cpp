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
const int NEIGHBORS[6][2] = {{1, 3}, {0, 2}, {1, 5}, {0, 4}, {3, 5}, {2, 4}};  // vl ml hl hr mr vr im Kreis

struct Cmd {
  float vx, vy, turn;
  const char* name;
};
const Cmd CMDS[] = {
    {1, 0, 0, "vor"},      {-1, 0, 0, "zurück"},  {0, 1, 0, "links"},
    {0, 0, 1, "drehen"},   {0, 0, -1, "drehen rechts"}, {0.7f, -0.7f, 0, "schräg"},
    {1, 0, 1, "Kurve"},    {-0.5f, 0.5f, -0.8f, "gemischt"},
    {0.25f, 0, 0, "langsam"}, {0, 0, 0.3f, "langsam drehen"},
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
  p.max_stride = gaitStrideMax(p.step_height);
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
  g.setSwingGuard(LEG_GEOMETRY, h, KNEE_MIN_DEG);
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
        p.max_stride = gaitStrideMax(p.step_height);
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
          if (g.swinging(i)) {
            ++swinging;
            CHECK(!g.swinging(NEIGHBORS[i][0]) && !g.swinging(NEIGHBORS[i][1]));  // nie zwei Nachbarn
          }
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
        // Anlaufen: das Bein mit dem spätesten Schwung setzt in Kurven ggf. vorher um
        // (Reichweite), dann auch einmal ein Bein mehr in der Luft
        if ((k + 1) * DT > 2 * gait::cycleTime(TYPES[ti], p.swing_s)) CHECK(swinging <= MAX_SWING[ti]);
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
  // Kniewinkel: Reichweitengrenze, vorzeitiges Umsetzen am Rand und Schwungbahn-Schutz
  CHECK(knee_min > KNEE_MIN_DEG - 0.5f);
  CHECK(ik_fail == 0);
  CHECK(limit_hits == 0);
  CHECK(max_slip < 0.05f);
  if (!extreme) {
    CHECK(max_jump < 9.0f);  // 450 mm/s bei 50 Hz
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

// Anfahren: die erste Beingruppe hebt sofort ab; Gangart-Wechsel im Stillstand sofort
static void testStartAndSwitch() {
  Vec3 n[6];
  neutralFeet(n, POSE_LIMITS.stance_default);
  gait::Gait g = makeGait(gait::Type::Tripod, n);
  g.setCommand(1, 0, 0);
  g.update(DT, n);
  g.update(DT, n);
  CHECK(g.swingMask() == 0b010101);  // vl, hl, mr

  g.setCommand(0, 0, 0);
  for (int k = 0; k < 300 && !g.idle(); ++k) g.update(DT, n);
  CHECK(g.idle());
  gait::Params p = g.params();
  p.type = gait::Type::Wave;
  g.setParams(p);
  CHECK(g.activeType() == gait::Type::Wave);
}

// Überwacht einen Lauf Schritt für Schritt: rutschen Standfüße (Welt), sind zwei
// Nachbarbeine gleichzeitig in der Luft, springen Füße, sind die Gelenke erreichbar?
struct GaitWatch {
  BodyPose pose{0, 0, POSE_LIMITS.height_default, 0, 0, 0};
  float bx = 0, by = 0, byaw = 0, pvx = 0, pvy = 0;
  Vec3 planted[6] = {}, prev[6] = {};
  bool was_stance[6] = {};
  float worst_acc = 0, worst_slip = 0, worst_jump = 0, knee_min = 1e9f;
  int adjacent = 0, too_many = 0, ik_fail = 0, limit_hits = 0, swinging = 0;

  explicit GaitWatch(const gait::Gait& g) {
    for (int i = 0; i < 6; ++i) prev[i] = g.foot(i);
    pvx = g.speedX();
    pvy = g.speedY();
  }
  void step(const gait::Gait& g) {
    worst_acc = std::fmax(worst_acc, std::hypot(g.speedX() - pvx, g.speedY() - pvy) / DT);
    pvx = g.speedX();
    pvy = g.speedY();
    float cy = std::cos(byaw), sy = std::sin(byaw);
    bx += (cy * g.speedX() - sy * g.speedY()) * DT;
    by += (sy * g.speedX() + cy * g.speedY()) * DT;
    byaw += g.turnRate() * DT;
    cy = std::cos(byaw);
    sy = std::sin(byaw);
    swinging = 0;
    for (int i = 0; i < 6; ++i) {
      const Vec3& f = g.foot(i);
      if (g.swinging(i)) {
        ++swinging;
        if (g.swinging(NEIGHBORS[i][0]) || g.swinging(NEIGHBORS[i][1])) ++adjacent;
      }
      worst_jump = std::fmax(worst_jump, std::hypot(std::hypot(f.x - prev[i].x, f.y - prev[i].y), f.z - prev[i].z));
      prev[i] = f;
      Vec3 w = {bx + cy * f.x - sy * f.y, by + sy * f.x + cy * f.y, f.z};
      // (im Stillstand rücken die Füße bis zu 2 mm auf die Neutralpunkte – gewollt)
      const bool stance = !g.swinging(i) && !g.idle();
      if (stance && was_stance[i]) worst_slip = std::fmax(worst_slip, std::hypot(w.x - planted[i].x, w.y - planted[i].y));
      if (stance && !was_stance[i]) planted[i] = w;
      was_stance[i] = stance;
      JointAngles a;
      if (!legIk(LEG_GEOMETRY, LEG_MOUNTS[i], bodyToLeg(LEG_MOUNTS[i], worldToBody(pose, f)), a)) {
        ++ik_fail;
        continue;
      }
      knee_min = std::fmin(knee_min, 90.0f + a.tibia);
      if (std::fabs(a.coxa) > 60 || a.femur < ASSEMBLY_POSE.femur - 70 || a.femur > ASSEMBLY_POSE.femur + 70 ||
          a.tibia < ASSEMBLY_POSE.tibia - 70 || a.tibia > ASSEMBLY_POSE.tibia + 70)
        ++limit_hits;
    }
    if (swinging > 3) ++too_many;
  }
  void check(float knee_tol) const {
    CHECK(adjacent == 0);
    CHECK(too_many == 0);
    CHECK(ik_fail == 0);
    CHECK(limit_hits == 0);
    CHECK(worst_slip < 0.05f);
    CHECK(worst_jump < 9.0f);
    CHECK(knee_min > KNEE_MIN_DEG - knee_tol);
  }
};

// Gangwechsel im Lauf (alle Richtungen, geradeaus, Kurve, seitwärts): fließend –
// Füße rutschen nicht, nie zwei Nachbarbeine gleichzeitig in der Luft, Gelenke
// erreichbar, Tempo ohne Sprung, danach eingeschwungen im neuen Muster
static void testWalkingSwitch() {
  Vec3 n[6];
  neutralFeet(n, POSE_LIMITS.stance_default);
  const Cmd SW_CMDS[] = {{1, 0, 0, "vor"}, {0.8f, 0, 0.6f, "Kurve"}, {0, 1, 0, "seitwärts"}};
  float worst_switch = 0, worst_conv = 0, worst_acc = 0, worst_jump = 0, knee_min = 1e9f;
  for (int from = 0; from < 3; ++from) {
    for (int to = 0; to < 3; ++to) {
      if (from == to) continue;
      for (const Cmd& c : SW_CMDS) {
        gait::Gait g = makeGait(TYPES[from], n);
        g.setCommand(c.vx, c.vy, c.turn);
        for (int k = 0; k < 200; ++k) g.update(DT, n);  // 4 s einschwingen
        gait::Params p = g.params();
        p.type = TYPES[to];
        g.setParams(p);
        GaitWatch w(g);
        float t_switch = -1, t_conv = -1;
        int over = 0;
        for (int k = 0; k < 600; ++k) {  // 12 s
          g.update(DT, n);
          w.step(g);
          const float t = (k + 1) * DT;
          if (t_switch < 0 && g.activeType() == TYPES[to]) t_switch = t;
          if (t_conv < 0 && !g.transitioning()) t_conv = t;
          // nach dem Einfinden: nie mehr Beine in der Luft, als die neue Gangart vorsieht
          if (t_conv > 0 && w.swinging > MAX_SWING[to]) ++over;
        }
        w.check(0.5f);
        CHECK(over == 0);
        CHECK(t_switch > 0 && t_switch < 1.5f);
        CHECK(t_conv > 0 && t_conv < 8.0f);
        CHECK(g.activeType() == TYPES[to] && !g.transitioning() && g.phaseError() == 0.0f);
        if (c.vx == 1 && c.turn == 0) CHECK_NEAR(g.speedX(), g.maxSpeed(), 0.5);  // wieder volles Tempo
        worst_switch = std::fmax(worst_switch, t_switch);
        worst_conv = std::fmax(worst_conv, t_conv);
        worst_acc = std::fmax(worst_acc, w.worst_acc);
        worst_jump = std::fmax(worst_jump, w.worst_jump);
        knee_min = std::fmin(knee_min, w.knee_min);
      }
    }
  }
  CHECK(worst_acc < 400.0f);  // mm/s²: Tempo gleitet, kein Sprung
  std::printf("Info: Gangwechsel im Lauf: umgetaktet nach max. %.1f s, eingeschwungen nach max. %.1f s, "
              "Beschleunigung max. %.0f mm/s², Knie min. %.1f°, Sprung max. %.2f mm\n",
              worst_switch, worst_conv, worst_acc, knee_min, worst_jump);
}

// Dauertest: zufällige Gangart-Wechsel und Fahrbefehle in schneller Folge (auch mitten
// im Umtakten, beim Anhalten und Anlaufen); am Ende Stillstand in der gewählten Gangart
static void testSwitchStress() {
  Vec3 n[6];
  neutralFeet(n, POSE_LIMITS.stance_default);
  uint32_t rnd = 12345;
  auto next = [&]() {
    rnd = rnd * 1664525u + 1013904223u;
    return (rnd >> 8) / 16777216.0f;  // 0..1
  };
  float knee_min = 1e9f, worst_jump = 0;
  for (int run = 0; run < 6; ++run) {
    gait::Gait g = makeGait(TYPES[run % 3], n);
    GaitWatch w(g);
    float t_next = 0;
    for (int k = 0; k < 3000; ++k) {  // 60 s
      const float t = k * DT;
      if (t >= t_next) {
        t_next = t + 0.2f + 2.5f * next();
        const float r = next();
        if (r < 0.45f) {
          gait::Params p = g.params();
          p.type = TYPES[static_cast<int>(next() * 2.999f)];
          g.setParams(p);
        } else if (r < 0.6f) {
          g.setCommand(0, 0, 0);
        } else if (r < 0.8f) {
          const Cmd& c = CMDS[static_cast<int>(next() * 9.999f)];
          g.setCommand(c.vx, c.vy, c.turn);
        } else {
          g.setCommand(2 * next() - 1, 2 * next() - 1, 2 * next() - 1);
        }
      }
      g.update(DT, n);
      w.step(g);
    }
    g.setCommand(0, 0, 0);
    for (int k = 0; k < 500 && !g.idle(); ++k) {
      g.update(DT, n);
      w.step(g);
    }
    w.check(0.5f);
    CHECK(g.idle() && g.activeType() == g.params().type && !g.transitioning());
    knee_min = std::fmin(knee_min, w.knee_min);
    worst_jump = std::fmax(worst_jump, w.worst_jump);
  }
  std::printf("Info: Dauertest Gangwechsel: Knie min. %.1f°, Sprung max. %.2f mm\n", knee_min, worst_jump);
}

// Fahren, loslassen, in eine andere Richtung weiter (zufällig, auch mit Drehen): Muss ein
// Bein am Rand der Reichweite auf seinen Nachbarn warten, bremst der Körper – kein Fuß
// außer Reichweite, das Knie knickt nicht ein
static void testRedirect() {
  Vec3 n[6];
  neutralFeet(n, POSE_LIMITS.stance_default);
  uint32_t rnd = 777;
  auto next = [&]() {
    rnd = rnd * 1664525u + 1013904223u;
    return (rnd >> 8) / 16777216.0f;
  };
  float knee_min = 1e9f;
  int ik_fail = 0, adjacent = 0;
  for (int sc = 0; sc < 3000; ++sc) {
    gait::Gait g = makeGait(TYPES[sc % 3], n);
    GaitWatch w(g);
    float a[3], b[3];
    for (int k = 0; k < 3; ++k) {
      a[k] = 2 * next() - 1;
      b[k] = 2 * next() - 1;
    }
    for (int k = 0; k < 3; ++k) {
      if (next() < 0.3f) a[k] = a[k] > 0 ? 1.0f : -1.0f;
      if (next() < 0.3f) b[k] = b[k] > 0 ? 1.0f : -1.0f;
    }
    const int t1 = 40 + static_cast<int>(next() * 160), rel = static_cast<int>(next() * 50);
    g.setCommand(a[0], a[1], a[2]);
    for (int k = 0; k < t1; ++k) {
      g.update(DT, n);
      w.step(g);
    }
    g.setCommand(0, 0, 0);
    for (int k = 0; k < rel; ++k) {
      g.update(DT, n);
      w.step(g);
    }
    g.setCommand(b[0], b[1], b[2]);
    for (int k = 0; k < 150; ++k) {
      g.update(DT, n);
      w.step(g);
    }
    knee_min = std::fmin(knee_min, w.knee_min);
    ik_fail += w.ik_fail;
    adjacent += w.adjacent;
  }
  CHECK(ik_fail == 0);
  CHECK(adjacent == 0);
  CHECK(knee_min > KNEE_MIN_DEG - 0.5f);
  std::printf("Info: 3000 Richtungswechsel: Knie min. %.1f°, IK-Fehler %d\n", knee_min, ik_fail);
}

// Grenzen ändern sich im Lauf (Richtung, Schrittdauer, Fußhub): das Tempo gleitet auf den
// neuen Höchstwert, statt in einem Regelschritt gekappt zu werden
static void testSmoothLimits() {
  Vec3 n[6];
  neutralFeet(n, POSE_LIMITS.stance_default);
  struct Case {
    int type;
    float a[3], b[3];
    int change;  // 0 Richtung, 1 Schrittdauer 0,3 -> 0,4 s, 2 Fußhub 15 -> 25 mm
  };
  const Case cases[] = {{0, {1, 0, 0}, {0, 1, 0}, 0}, {1, {1, 0, 0}, {0, 1, 0}, 0}, {0, {1, 0, 0}, {-1, 0, 0}, 0},
                        {0, {1, 0, 0}, {1, 0, 0}, 1}, {0, {1, 0, 0}, {1, 0, 0}, 2}};
  float worst = 0;
  for (const Case& c : cases) {
    gait::Params p = firmwareParams(TYPES[c.type]);
    if (c.change == 1) p.swing_s = 0.3f;
    gait::Gait g = makeGait(TYPES[c.type], n, &p);
    g.setCommand(c.a[0], c.a[1], c.a[2]);
    for (int k = 0; k < 200; ++k) g.update(DT, n);
    GaitWatch w(g);
    if (c.change == 1) {
      p.swing_s = 0.4f;
      g.setParams(p);
    }
    if (c.change == 2) {
      p.step_height = GAIT_LIMITS.height_max;
      p.max_stride = gaitStrideMax(p.step_height);
      g.setParams(p);
      setReach(g, p.step_height);
    }
    g.setCommand(c.b[0], c.b[1], c.b[2]);
    for (int k = 0; k < 200; ++k) {
      g.update(DT, n);
      w.step(g);
    }
    w.check(0.5f);
    worst = std::fmax(worst, w.worst_acc);
  }
  CHECK(worst < 1000.0f);  // vorher bis 4800 mm/s² (Tempo in einem Regelschritt gekappt)
  std::printf("Info: Grenzen ändern sich im Lauf: Beschleunigung max. %.0f mm/s²\n", worst);
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

// Schrittfrequenz: langsam -> ruhigere, längere Schritte; schnell -> eingestellte Schrittdauer
static void testCadence() {
  Vec3 n[6];
  neutralFeet(n, POSE_LIMITS.stance_default);
  auto run = [&](float vx, int& swings, float& cadence, float& speed) {
    gait::Gait g = makeGait(gait::Type::Tripod, n);
    g.setCommand(vx, 0, 0);
    for (int k = 0; k < 150; ++k) g.update(DT, n);  // einschwingen (3 s)
    swings = 0;
    uint8_t prev = g.swingMask();
    for (int k = 0; k < 250; ++k) {  // 5 s zählen
      g.update(DT, n);
      uint8_t m = g.swingMask();
      swings += __builtin_popcount(m & ~prev);  // neu abgehobene Beine
      prev = m;
    }
    cadence = g.cadence();
    speed = g.speedX();
  };
  int sw_fast, sw_slow;
  float cad_fast, cad_slow, v_fast, v_slow;
  run(1.0f, sw_fast, cad_fast, v_fast);
  run(0.2f, sw_slow, cad_slow, v_slow);
  CHECK_NEAR(cad_fast, 1.0, 1e-3);
  CHECK_NEAR(cad_slow, 2.0, 1e-3);
  CHECK_NEAR(v_slow, 0.2 * v_fast, 0.5);              // Tempo bleibt wie befohlen
  CHECK(sw_slow * 10 < sw_fast * 6);                  // deutlich weniger Schritte
  // Schrittlänge langsam = 0.2 * 125 mm/s * Standphase 0,8 s = 20 mm statt 10 mm
  const float stance_slow = gait::dutyFactor(gait::Type::Tripod) *
                            gait::cycleTime(gait::Type::Tripod, GAIT_LIMITS.swing_default * cad_slow);
  CHECK(v_slow * stance_slow > 0.35f * GAIT_LIMITS.stride_max);
  std::printf("Info: Schritte in 5 s: schnell %d, langsam (20 %%) %d, Schrittdauer langsam %.2f s\n", sw_fast,
              sw_slow, GAIT_LIMITS.swing_default * cad_slow);
}

int main() {
  testProfiles();
  testIdle();
  testWalking(false);
  testWalking(true);
  testStop();
  testStartAndSwitch();
  testWalkingSwitch();
  testSwitchStress();
  testRedirect();
  testSmoothLimits();
  testStartExcursion();
  testCadence();
  std::printf("%d Prüfungen, %d Fehler\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
