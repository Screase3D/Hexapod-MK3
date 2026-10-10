// Tests der Kinematik auf dem PC (ohne ESP32):
//   g++ -std=c++17 -I../src -I../include kinematics_test.cpp ../src/kinematics/kinematics.cpp -o kin_test && ./kin_test
// Läuft auch in der CI (.github/workflows/firmware.yml).

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "kinematics/kinematics.h"
#include "robot_geometry.h"

using namespace kin;

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      ++g_failed;                                                       \
      std::printf("FEHLER %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                   \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                          \
  do {                                                                                 \
    ++g_checks;                                                                        \
    double _a = (a), _b = (b);                                                         \
    if (std::fabs(_a - _b) > (tol)) {                                                  \
      ++g_failed;                                                                      \
      std::printf("FEHLER %s:%d: %s = %.4f, erwartet %.4f\n", __FILE__, __LINE__, #a, _a, _b); \
    }                                                                                  \
  } while (0)

static const LegGeometry G = {30.0f, 60.0f, 90.0f};
static const LegMount MOUNTS[] = {
    {60, 40, 45},   {0, 50, 90},   {-60, 40, 135},     // links
    {60, -40, -45}, {0, -50, -90}, {-60, -40, -135},   // rechts
};

static float rnd(float lo, float hi) { return lo + (hi - lo) * (std::rand() / (float)RAND_MAX); }

static void testNeutralPose() {
  for (const auto& m : MOUNTS) {
    JointAngles a{};
    CHECK(legIk(G, m, {G.coxa + G.femur, 0, -G.tibia}, a));
    CHECK_NEAR(a.coxa, 0, 1e-3);
    CHECK_NEAR(a.femur, 0, 1e-3);
    CHECK_NEAR(a.tibia, 0, 1e-3);
  }
}

static void testFkOfZero() {
  Vec3 p = legFk(G, MOUNTS[0], {0, 0, 0});
  CHECK_NEAR(p.x, G.coxa + G.femur, 1e-3);
  CHECK_NEAR(p.y, 0, 1e-3);
  CHECK_NEAR(p.z, -G.tibia, 1e-3);
}

static void testRoundTripAngles() {
  std::srand(42);
  for (const auto& m : MOUNTS) {
    for (int i = 0; i < 2000; ++i) {
      JointAngles a{rnd(-60, 60), rnd(-70, 70), rnd(-80, 80)};
      Vec3 p = legFk(G, m, a);
      // Nur Stellungen, bei denen der Fuß außerhalb der Coxa-Achse liegt.
      // Liegt er dahinter (unter dem Körper), ist die Coxa um 180° mehrdeutig.
      float f = a.femur * 3.14159265f / 180.0f, k = (a.tibia + 90.0f) * 3.14159265f / 180.0f;
      float r_signed = G.coxa + G.femur * std::cos(f) + G.tibia * std::cos(f + k - 3.14159265f);
      if (r_signed < 5.0f) continue;
      JointAngles b{};
      bool ok = legIk(G, m, p, b);
      CHECK(ok);
      if (!ok) continue;
      CHECK_NEAR(b.coxa, a.coxa, 0.05);
      CHECK_NEAR(b.femur, a.femur, 0.05);
      CHECK_NEAR(b.tibia, a.tibia, 0.05);
    }
  }
}

static void testSignConventions() {
  // Femur + hebt den Fuß, Tibia + bewegt ihn nach außen
  Vec3 p0 = legFk(G, MOUNTS[1], {0, 0, 0});
  CHECK(legFk(G, MOUNTS[1], {0, 10, 0}).z > p0.z);
  CHECK(legFk(G, MOUNTS[1], {0, 0, 10}).x > p0.x);

  // Coxa + bewegt den Fuß auf BEIDEN Seiten nach vorne (+x im Körper)
  for (const auto& m : MOUNTS) {
    Vec3 base = legToBody(m, legFk(G, m, {0, 0, 0}));
    Vec3 fwd = legToBody(m, legFk(G, m, {10, 0, 0}));
    CHECK(fwd.x > base.x);
  }

  // Ein Fußpunkt weiter vorne ergibt eine positive Coxa
  for (const auto& m : MOUNTS) {
    Vec3 foot_body = legToBody(m, {G.coxa + G.femur, 0, -G.tibia});
    foot_body.x += 15.0f;
    JointAngles a{};
    CHECK(legIk(G, m, bodyToLeg(m, foot_body), a));
    CHECK(a.coxa > 0);
  }
}

static void testUnreachable() {
  JointAngles a{1, 2, 3};
  CHECK(!legIk(G, MOUNTS[0], {500, 0, 0}, a));
  CHECK(!legIk(G, MOUNTS[0], {0, 0, -50}, a));  // genau unter der Coxa-Achse
  CHECK_NEAR(a.coxa, 1, 0);  // bei Fehler unverändert
}

static void testBodyLegTransform() {
  for (const auto& m : MOUNTS) {
    Vec3 p{12, -34, 56};
    Vec3 q = legToBody(m, bodyToLeg(m, p));
    CHECK_NEAR(q.x, p.x, 1e-3);
    CHECK_NEAR(q.y, p.y, 1e-3);
    CHECK_NEAR(q.z, p.z, 1e-3);
  }
  // Standard-Fußpunkt liegt im Bein-System auf der x-Achse
  Vec3 f = bodyToLeg(MOUNTS[0], stanceFoot(MOUNTS[0], 80));
  CHECK_NEAR(f.x, 80, 1e-3);
  CHECK_NEAR(f.y, 0, 1e-3);
}

static void testWorldToBody() {
  const float h = 70;
  BodyPose level{0, 0, h, 0, 0, 0};
  Vec3 p = worldToBody(level, {100, 20, 0});
  CHECK_NEAR(p.x, 100, 1e-3);
  CHECK_NEAR(p.y, 20, 1e-3);
  CHECK_NEAR(p.z, -h, 1e-3);

  // Gieren +90° (nach links): ein Punkt vorne liegt danach rechts vom Körper
  BodyPose yaw{0, 0, h, 0, 0, 90};
  p = worldToBody(yaw, {100, 0, 0});
  CHECK_NEAR(p.x, 0, 1e-3);
  CHECK_NEAR(p.y, -100, 1e-3);

  // Rollen + (rechte Seite runter): rechter Fuß rückt näher an den Körper
  BodyPose roll{0, 0, h, 10, 0, 0};
  CHECK(worldToBody(roll, {0, -100, 0}).z > -h);
  CHECK(worldToBody(roll, {0, 100, 0}).z < -h);

  // Nicken + (Nase hoch): vorderer Fuß liegt tiefer unter dem Körper
  BodyPose pitch{0, 0, h, 0, 10, 0};
  CHECK(worldToBody(pitch, {100, 0, 0}).z < -h);
  CHECK(worldToBody(pitch, {-100, 0, 0}).z > -h);

  // Verschieben nach vorne: Füße wandern relativ nach hinten
  BodyPose shift{15, 0, h, 0, 0, 0};
  CHECK_NEAR(worldToBody(shift, {100, 0, 0}).x, 85, 1e-3);
}

// Mit den Maßen aus robot_geometry.h muss die Standardhaltung für alle Beine
// erreichbar sein und innerhalb plausibler Gelenkwinkel liegen.
static void testDefaultPose() {
  BodyPose pose{0, 0, POSE_LIMITS.height_default, 0, 0, 0};
  for (int leg = 0; leg < 6; ++leg) {
    const LegMount& m = LEG_MOUNTS[leg];
    Vec3 foot = bodyToLeg(m, worldToBody(pose, stanceFoot(m, POSE_LIMITS.stance_default)));
    JointAngles a{};
    CHECK(legIk(LEG_GEOMETRY, m, foot, a));
    CHECK(std::fabs(a.coxa) < 1.0f);  // Füße stehen in Montagerichtung
    CHECK(std::fabs(a.femur) < 80.0f);
    CHECK(std::fabs(a.tibia) < 80.0f);
  }
  // Extremwerte der Oberfläche: nur informativ ausgeben
  int unreachable = 0, total = 0;
  const float zs[] = {POSE_LIMITS.height_min, POSE_LIMITS.height_max};
  const float ss[] = {POSE_LIMITS.stance_min, POSE_LIMITS.stance_max};
  for (float z : zs)
    for (float st : ss)
      for (int leg = 0; leg < 6; ++leg) {
        BodyPose p{0, 0, z, 0, 0, 0};
        const LegMount& m = LEG_MOUNTS[leg];
        JointAngles a{};
        ++total;
        if (!legIk(LEG_GEOMETRY, m, bodyToLeg(m, worldToBody(p, stanceFoot(m, st))), a)) ++unreachable;
      }
  std::printf("Info: %d von %d Extremstellungen (Höhe/Fußabstand) nicht erreichbar\n", unreachable, total);
}

int main() {
  testNeutralPose();
  testFkOfZero();
  testRoundTripAngles();
  testSignConventions();
  testUnreachable();
  testBodyLegTransform();
  testWorldToBody();
  testDefaultPose();
  std::printf("%d Prüfungen, %d Fehler\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
