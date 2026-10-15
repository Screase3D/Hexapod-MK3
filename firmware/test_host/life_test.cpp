// Tests für "Lebendig wirken" (Stimmung, Atmen, Hochheben, Zappeln, Laufen) auf dem PC:
//   g++ -std=c++17 -I../src -I../include life_test.cpp ../src/kinematics/life.cpp ../src/kinematics/alive.cpp
//       ../src/kinematics/gestures.cpp ../src/kinematics/gait.cpp ../src/kinematics/kinematics.cpp -o life_test
// Läuft auch in der CI (.github/workflows/firmware.yml).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "kinematics/life.h"

using namespace life;

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

constexpr float DT = 0.02f;

// Langeweile -> einschlafen -> aufwachen
static void testMoods() {
  Moods m(1234);
  const float sleep_s = 100.0f;
  MoodIn in;
  in.dt = DT;
  in.standing = in.still = true;
  int yawns = 0, others = 0, sits = 0;
  float bored_at = -1, sit_at = -1;
  for (int k = 0; k < (int)(130 / DT); ++k) {
    MoodOut o = m.update(in, sleep_s);
    const float t = k * DT;
    if (o.play == alive::Action::Yawn) ++yawns;
    else if (o.play != alive::Action::None) ++others;
    if (m.mood() == Mood::Bored && bored_at < 0) bored_at = t;
    if (o.sit) {
      ++sits;
      sit_at = t;
      in.standing = in.still = false;  // setzt sich
    }
  }
  CHECK(bored_at > 39.0f && bored_at < 41.0f);
  CHECK(sits == 1 && sit_at > 100.0f && sit_at < 106.0f);
  CHECK(yawns >= 2);  // beim Gelangweiltwerden und vor dem Einschlafen
  CHECK(m.mood() == Mood::Asleep);
  std::printf("Info: gelangweilt nach %.0f s, eingeschlafen nach %.1f s, %d× gähnen, %d andere\n", bored_at, sit_at,
              yawns, others);

  // Eingabe ohne Bewegungswunsch: wach, bleibt sitzen
  in.activity = true;
  MoodOut o = m.update(in, sleep_s);
  CHECK(!o.stand && m.mood() == Mood::Awake);
  in.activity = false;
  CHECK(!m.update(in, sleep_s).stand);
  // erneut einschlafen lassen, dann mit Bewegungswunsch wecken
  Moods m2(7);
  in = MoodIn();
  in.dt = DT;
  in.standing = in.still = true;
  for (int k = 0; k < (int)(110 / DT); ++k) {
    if (m2.update(in, sleep_s).sit) in.standing = in.still = false;
  }
  CHECK(m2.mood() == Mood::Asleep);
  in.activity = in.move = true;
  o = m2.update(in, sleep_s);
  CHECK(o.stand && m2.mood() == Mood::Awake);
  in.activity = in.move = false;
  // setzt sich noch hin (Aufstehen geht erst danach): Wunsch bleibt bestehen
  for (int k = 0; k < 50; ++k) CHECK(m2.update(in, sleep_s).stand);
  // steht wieder: streckt sich einmal
  in.standing = in.still = true;
  o = m2.update(in, sleep_s);
  CHECK(o.play == alive::Action::Stretch);
  o = m2.update(in, sleep_s);
  CHECK(o.play == alive::Action::None);

  // Eingaben halten ihn wach
  Moods m3(9);
  in = MoodIn();
  in.dt = DT;
  in.standing = in.still = true;
  for (int k = 0; k < (int)(300 / DT); ++k) {
    in.activity = k % (int)(30 / DT) == 0;
    MoodOut x = m3.update(in, sleep_s);
    CHECK(!x.sit);
  }
  CHECK(m3.mood() == Mood::Awake);
  // ausgeschaltet: nie gelangweilt
  Moods m4(3);
  in.activity = false;
  in.enabled = false;
  for (int k = 0; k < (int)(300 / DT); ++k) CHECK(!m4.update(in, sleep_s).sit);
  CHECK(m4.mood() == Mood::Awake);
}

static void testBreath() {
  Breath b;
  float lo = 0, hi = 0, last = 0, max_step = 0;
  for (int k = 0; k < (int)(12 / DT); ++k) {
    float z = b.update(DT, true);
    lo = std::fmin(lo, z);
    hi = std::fmax(hi, z);
    max_step = std::fmax(max_step, std::fabs(z - last));
    last = z;
  }
  CHECK(hi > 1.8f && hi <= 2.0f && lo < -1.8f && lo >= -2.0f);
  CHECK(max_step < 0.1f);  // weich
  for (int k = 0; k < (int)(2 / DT); ++k) last = b.update(DT, false);
  CHECK(std::fabs(last) < 1e-4f && std::fabs(b.wave()) < 1e-4f);
}

static float noise(float amp) { return amp * ((std::rand() % 2001) / 1000.0f - 1.0f); }

// Ticks bis zur Erkennung (-1 = nie) für einen Verlauf: fn(k, in) setzt die Messwerte
template <typename F>
static int liftAfter(LiftDetector& d, int ticks, uint8_t sens, F fn) {
  for (int k = 0; k < ticks; ++k) {
    LiftIn in;
    in.dt = DT;
    in.armed = true;
    in.sens = sens;
    fn(k, in);
    if (d.update(in)) return k;
  }
  return -1;
}

static void still(LiftIn& in, float a = 0.02f, float g = 3.0f) {
  in.acc[0] = noise(a);
  in.acc[1] = noise(a);
  in.acc[2] = 1.0f + noise(a);
  for (float& v : in.gyro) v = noise(g);
  in.tilt_err[0] = noise(0.3f);
  in.tilt_err[1] = noise(0.3f);
}

static void testLift() {
  std::srand(42);
  // eine Minute ruhig stehen (Rauschen, Servozittern): nie – auch nicht bei hoher Empfindlichkeit
  for (uint8_t sens = 1; sens <= 3; ++sens) {
    LiftDetector d;
    CHECK(liftAfter(d, (int)(60 / DT), sens, [&](int, LiftIn& in) { still(in, sens == 3 ? 0.02f : 0.03f, 4.0f); }) < 0);
  }
  // auf einer Schräge stehen (Neigung weicht dauerhaft 5° ab): nie
  {
    LiftDetector d;
    CHECK(liftAfter(d, (int)(20 / DT), 2, [](int, LiftIn& in) { still(in); in.tilt_err[0] += 5.0f; }) < 0);
  }
  // einzelner harter Stoß (ein Takt): nein
  {
    LiftDetector d;
    CHECK(liftAfter(d, 60, 2, [](int k, LiftIn& in) { still(in); if (k == 40) in.acc[2] = 2.0f; }) < 0);
  }
  // sanft anheben: +0,15 g für 200 ms -> nach 2–3 Takten
  int at;
  {
    LiftDetector d;
    at = liftAfter(d, 60, 2, [](int k, LiftIn& in) { still(in); if (k >= 30 && k < 40) in.acc[2] += 0.15f; });
    CHECK(at >= 31 && at <= 33);
    std::printf("Info: sanftes Anheben erkannt nach %d ms\n", (at - 30 + 1) * 20);
  }
  // langsam in der Hand gekippt (10° in 0,5 s, kaum Beschleunigung, 20 °/s): ja
  {
    LiftDetector d;
    at = liftAfter(d, 100, 2, [](int k, LiftIn& in) {
      still(in);
      if (k >= 30) in.tilt_err[1] = std::fmin(10.0f, (k - 30) * DT * 20.0f);
    });
    CHECK(at > 30 && at <= 56);
    std::printf("Info: langsames Kippen erkannt nach %d ms\n", (at - 30 + 1) * 20);
  }
  // in der Hand gedreht (Gyro 40 °/s): ja; während einer eigenen Bewegung ("Lebendig") nicht
  {
    LiftDetector d;
    at = liftAfter(d, 60, 2, [](int k, LiftIn& in) { still(in); if (k >= 30) in.gyro[2] = 40.0f; });
    CHECK(at >= 31 && at <= 34);
    std::printf("Info: Drehen in der Hand erkannt nach %d ms\n", (at - 30 + 1) * 20);
    LiftDetector d2;
    CHECK(liftAfter(d2, 100, 2, [](int k, LiftIn& in) { still(in); in.moving = true; if (k >= 30) in.gyro[2] = 40.0f; }) < 0);
  }
  // geringe Empfindlichkeit: sanftes Anheben nicht, kräftiges schon
  {
    LiftDetector d;
    CHECK(liftAfter(d, 60, 1, [](int k, LiftIn& in) { still(in); if (k >= 30 && k < 40) in.acc[2] += 0.15f; }) < 0);
    LiftDetector d2;
    CHECK(liftAfter(d2, 60, 1, [](int k, LiftIn& in) { still(in); if (k >= 30 && k < 40) in.acc[2] += 0.3f; }) > 0);
  }
  // nicht scharf (läuft gerade): nie; gerade erst scharf: 0,4 s Schonfrist
  {
    LiftDetector d;
    LiftIn in;
    in.dt = DT;
    in.acc[2] = 1.5f;
    for (int k = 0; k < 100; ++k) CHECK(!d.update(in));
    in.armed = true;
    bool hit = false;
    for (int k = 0; k < (int)(0.38f / DT); ++k) hit |= d.update(in);
    CHECK(!hit);
  }
}

static void testFlail() {
  Flail f;
  CHECK(!f.active());
  f.start();
  float max_up = 0;
  int both = 0;
  float t = 0;
  for (; t < 5.0f && f.active(); t += DT) {
    alive::Output o = f.update(DT);
    bool a = false, b = false;
    for (int leg = 0; leg < 6; ++leg) {
      max_up = std::fmax(max_up, o.foot[leg].z);
      CHECK(o.foot[leg].z >= 0.0f);
      if (o.foot[leg].z > 0.5f) (leg % 2 == 0 ? a : b) = true;
    }
    both += a && b;
  }
  CHECK(both == 0);  // nie beide Dreiergruppen in der Luft
  CHECK(max_up > 20.0f && max_up <= 24.0f);
  CHECK(!f.active() && t > 2.2f && t < 2.8f);
  alive::Output o = f.update(DT);
  for (int leg = 0; leg < 6; ++leg) CHECK(o.foot[leg].z == 0.0f);
  // vorzeitig beenden: blendet in 0,25 s aus
  f.start();
  for (int k = 0; k < 30; ++k) f.update(DT);
  f.stop();
  int k = 0;
  while (f.active() && k < 100) {
    f.update(DT);
    ++k;
  }
  CHECK(k <= 14);
}

static void testWalkFx() {
  WalkFx w;
  WalkIn in;
  in.dt = DT;
  in.max_speed = 120;
  in.max_turn = 40;
  // anfahren mit 400 mm/s² bis 100 mm/s: Körper bleibt zurück (x < 0)
  float min_x = 0, max_x = 0;
  for (int k = 0; k < (int)(3 / DT); ++k) {
    in.on = true;
    in.vx = std::fmin(100.0f, 400.0f * k * DT);
    in.phase = std::fmod(k * DT / 0.8f, 1.0f);
    gestures::Offset o = w.update(in);
    min_x = std::fmin(min_x, o.x);
    CHECK(std::fabs(o.x) <= 7.0f && std::fabs(o.roll) <= 1.6f && std::fabs(o.z) <= 1.2f);
    CHECK(o.pitch <= 0.0f);  // vorwärts: Nase etwas runter
  }
  CHECK(min_x < -3.0f);
  // gleichmäßig: Auslenkung klingt ab, Hüftschwung bleibt
  gestures::Offset o = w.update(in);
  CHECK(std::fabs(o.x) < 0.5f);
  // anhalten: nach vorne, dann leicht zurück (nachschwingen)
  min_x = 0;
  bool stopped = false;
  for (int k = 0; k < (int)(3 / DT); ++k) {
    in.vx = std::fmax(0.0f, 100.0f - 400.0f * k * DT);
    in.on = in.vx > 0.0f;
    o = w.update(in);
    stopped |= w.stopped();
    max_x = std::fmax(max_x, o.x);
    if (max_x > 2.0f) min_x = std::fmin(min_x, o.x);
  }
  CHECK(max_x > 3.0f && min_x < -0.2f);
  CHECK(stopped);
  CHECK(std::fabs(o.x) < 0.5f && std::fabs(o.roll) < 0.1f && std::fabs(o.yaw) < 0.5f);
  // Kurve links: Blick nach links (yaw > 0)
  in.on = true;
  in.vx = 60;
  in.wz = 40;
  for (int k = 0; k < (int)(2 / DT); ++k) o = w.update(in);
  CHECK(o.yaw > 7.0f && o.yaw <= 9.5f);
  // seitwärts rechts: Blick nach rechts
  in.vx = 0;
  in.vy = -80;
  in.wz = 0;
  for (int k = 0; k < (int)(2 / DT); ++k) o = w.update(in);
  CHECK(o.yaw < -4.0f && o.yaw >= -6.1f);
  CHECK(gaze(1.0f, 0.0f, 0.0f) == 1.0f && gaze(0.0f, -1.0f, 0.0f) < -0.5f && gaze(0, 0, 0) == 0.0f);
}

// Langeweile-Aktionen in "Lebendig": spielen auch ohne "Lebendig", enden bei null
static void testAliveActions() {
  static const kin::LegMount mounts[6] = {};
  for (alive::Action a : {alive::Action::Yawn, alive::Action::Sigh, alive::Action::Stretch}) {
    alive::Animator an(mounts, 5);
    an.setBreath(false);
    an.play(a);
    CHECK(an.playing());
    float max_pitch = 0, max_z = 0;
    int k = 0;
    for (; k < 1000 && an.playing(); ++k) {
      alive::Output o = an.update(DT, false, true);
      max_pitch = std::fmax(max_pitch, std::fabs(o.body.pitch));
      max_z = std::fmax(max_z, std::fabs(o.body.z));
    }
    CHECK(!an.playing() && k > 150 && k < 700);
    CHECK(max_pitch >= 10.0f && max_pitch <= 18.0f && max_z >= 8.0f && max_z <= 19.0f);  // deutlich sichtbar
    alive::Output o;
    for (int i = 0; i < 40; ++i) o = an.update(DT, false, true);
    CHECK(std::fabs(o.body.z) < 1e-3f && std::fabs(o.body.pitch) < 1e-3f);
    CHECK(an.silent());
    std::printf("Info: %s %.1f s\n", alive::actionName(a), k * DT);
  }
  // nicht erlaubt (läuft gerade): bricht ab
  alive::Animator an(mounts, 5);
  an.play(alive::Action::Yawn);
  an.update(DT, false, false);
  CHECK(!an.playing());
}

int main() {
  testMoods();
  testBreath();
  testLift();
  testFlail();
  testWalkFx();
  testAliveActions();
  std::printf("%d Prüfungen, %d Fehler\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
