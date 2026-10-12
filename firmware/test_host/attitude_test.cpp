// Tests für Lagesensor-Mathematik und Lageausgleich auf dem PC:
//   g++ -std=c++17 -Wall -Wextra -Werror -I../src -I../include attitude_test.cpp ../src/sensors/attitude.cpp ../src/kinematics/leveling.cpp -o attitude_test && ./attitude_test

#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "kinematics/leveling.h"
#include "sensors/attitude.h"

using namespace attitude;

static int g_checks = 0, g_failed = 0;
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

constexpr float DEG = 3.14159265f / 180.0f;

// Schwerkraft-Messwert im Körpersystem bei gegebener Lage (Grad):
// Nase hoch -> Vorne-Achse zeigt nach oben -> a.x > 0; rechte Seite runter -> a.y > 0
static V3 gravityBody(float roll, float pitch) {
  float r = roll * DEG, p = pitch * DEG;
  return {std::sin(p), std::cos(p) * std::sin(r), std::cos(p) * std::cos(r)};
}

// Körper -> Sensor (Umkehrung von toBody, über die Basisvektoren)
static V3 toSensor(const Mount& m, const V3& b) {
  V3 ex = toBody(m, {1, 0, 0}), ey = toBody(m, {0, 1, 0}), ez = toBody(m, {0, 0, 1});
  return {b.x * ex.x + b.y * ex.y + b.z * ex.z, b.x * ey.x + b.y * ey.y + b.z * ey.z,
          b.x * ez.x + b.y * ez.y + b.z * ez.z};
}

// Für jede Einbaulage: Sensorwerte einer bekannten Lage zurückrechnen -> gleiche Lage
static void testMounts() {
  for (int d = 0; d < 4; ++d) {
    for (int flip = 0; flip < 2; ++flip) {
      Mount m{static_cast<AxisDir>(d), flip == 1};
      // Rechtssystem bleibt erhalten (keine Spiegelung)
      V3 x = toBody(m, {1, 0, 0}), y = toBody(m, {0, 1, 0}), z = toBody(m, {0, 0, 1});
      V3 c = {x.y * y.z - x.z * y.y, x.z * y.x - x.x * y.z, x.x * y.y - x.y * y.x};
      CHECK_NEAR(c.x, z.x, 1e-6);
      CHECK_NEAR(c.y, z.y, 1e-6);
      CHECK_NEAR(c.z, z.z, 1e-6);
      for (float roll : {-20.0f, 0.0f, 12.0f}) {
        for (float pitch : {-15.0f, 0.0f, 25.0f}) {
          V3 s = toSensor(m, gravityBody(roll, pitch));
          V3 b = toBody(m, s);
          CHECK_NEAR(accRollDeg(b), roll, 1e-3);
          CHECK_NEAR(accPitchDeg(b), pitch, 1e-3);
        }
      }
    }
  }
  // Pfeil nach vorne, Bauteile oben: Sensor = Körper
  V3 b = toBody(Mount{}, {0.1f, 0.2f, 0.9f});
  CHECK_NEAR(b.x, 0.1, 1e-6);
  CHECK_NEAR(b.y, 0.2, 1e-6);
  CHECK_NEAR(b.z, 0.9, 1e-6);
  AxisDir d;
  CHECK(axisDirFromName("hinten", d) && d == AxisDir::Backward);
  CHECK(!axisDirFromName("oben", d));
}

// Drehrate und Schwerkraft passen zusammen: Nase hochdrehen (Drehung um -y)
// lässt den Filter schon ohne Beschleunigungs-Anteil positiv werden
static void testFilterRates() {
  Filter f;
  f.reset(gravityBody(0, 0));
  for (int i = 0; i < 50; ++i) f.update(gravityBody(0, 0), {0, -10, 0}, 0.02f, 1.0f);
  CHECK_NEAR(f.pitchDeg(), 10.0, 1e-3);  // 10 °/s * 1 s
  f.reset(gravityBody(0, 0));
  for (int i = 0; i < 50; ++i) f.update(gravityBody(0, 0), {10, 0, 0}, 0.02f, 1.0f);
  CHECK_NEAR(f.rollDeg(), 10.0, 1e-3);  // Drehung um +x hebt die linke Seite
  // mit Beschleunigung: läuft auf die echte Lage zu
  f.reset(gravityBody(0, 0));
  for (int i = 0; i < 500; ++i) f.update(gravityBody(8, -5), {0, 0, 0}, 0.02f, 0.98f);
  CHECK_NEAR(f.rollDeg(), 8.0, 0.05);
  CHECK_NEAR(f.pitchDeg(), -5.0, 0.05);
}

static void testStats() {
  Stats s;
  for (int i = 0; i < 100; ++i) s.add({1.0f + (i % 2 ? 0.1f : -0.1f), 2.0f, -3.0f});
  CHECK(s.count() == 100);
  CHECK_NEAR(s.mean().x, 1.0, 1e-5);
  CHECK_NEAR(s.mean().z, -3.0, 1e-5);
  CHECK_NEAR(s.maxStdDev(), 0.1, 1e-4);
}

// Regelstrecke: gemessene Lage = Untergrund + befohlene Neigung (sign = +1),
// verzögert wie Servos und Filter (ca. 0,2 s)
struct Plant {
  float terrain_roll, terrain_pitch, sign;
  float roll = 0, pitch = 0;
  void step(float dt, float cmd_roll, float cmd_pitch) {
    float a = dt / 0.2f;
    roll += a * (terrain_roll + sign * cmd_roll - roll);
    pitch += a * (terrain_pitch + sign * cmd_pitch - pitch);
  }
};

static void testLeveling() {
  const float dt = 0.02f;
  // richtig herum: gleicht 8° / -6° Schräglage aus, Nutzer-Neigung bleibt Soll
  {
    leveling::Controller c;
    Plant p{8.0f, -6.0f, 1.0f};
    const float target_roll = 0.0f, target_pitch = 3.0f;
    for (int i = 0; i < 400; ++i) {  // 8 s
      c.update(dt, true, p.roll, p.pitch, target_roll, target_pitch);
      p.step(dt, target_roll + c.rollDeg(), target_pitch + c.pitchDeg());
    }
    CHECK(!c.fault());
    CHECK_NEAR(p.roll, target_roll, 0.6);
    CHECK_NEAR(p.pitch, target_pitch, 0.6);
    CHECK_NEAR(c.rollDeg(), -8.0, 0.6);
    // abschalten: Korrektur baut sich ab
    for (int i = 0; i < 100; ++i) c.update(dt, false, p.roll, p.pitch, 0, 0);
    CHECK_NEAR(c.rollDeg(), 0.0, 1e-6);
    CHECK_NEAR(c.pitchDeg(), 0.0, 1e-6);
  }
  // steiler Hang (20°): Korrektur am Anschlag, aber kein Fehler
  {
    leveling::Controller c;
    Plant p{20.0f, 0.0f, 1.0f};
    for (int i = 0; i < 500; ++i) {
      c.update(dt, true, p.roll, p.pitch, 0, 0);
      p.step(dt, c.rollDeg(), c.pitchDeg());
    }
    CHECK(!c.fault());
    CHECK_NEAR(c.rollDeg(), -15.0, 1e-3);
  }
  // falsch herum eingebauter Sensor: Regler schaltet ab und baut die Korrektur ab
  {
    leveling::Controller c;
    Plant p{3.0f, 0.0f, -1.0f};
    int i = 0;
    for (; i < 1000 && !c.fault(); ++i) {
      c.update(dt, true, p.roll, p.pitch, 0, 0);
      p.step(dt, c.rollDeg(), c.pitchDeg());
    }
    CHECK(c.fault());
    CHECK(i * dt < 10.0f);
    for (int k = 0; k < 200; ++k) c.update(dt, true, p.roll, p.pitch, 0, 0);
    CHECK_NEAR(c.rollDeg(), 0.0, 1e-6);
    std::printf("Info: falsche Sensorrichtung nach %.1f s erkannt\n", i * dt);
  }
  // kleine Abweichungen (Totzone) bewegen nichts
  {
    leveling::Controller c;
    for (int i = 0; i < 200; ++i) c.update(dt, true, 0.3f, -0.4f, 0, 0);
    CHECK_NEAR(c.rollDeg(), 0.0, 1e-6);
    CHECK_NEAR(c.pitchDeg(), 0.0, 1e-6);
  }
}

int main() {
  testMounts();
  testFilterRates();
  testStats();
  testLeveling();
  std::printf("%d Prüfungen, %d Fehler\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
