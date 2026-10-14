// Tests für den Controller (Bluetooth-Bericht, Belegung, Modi):
//   g++ -std=c++17 -Wall -Wextra -Werror -I../src -I../include pad_test.cpp ../src/input/pad.cpp ../src/input/pad_map.cpp -o pad_test && ./pad_test

#include <cmath>
#include <cstdio>
#include <cstring>

#include "input/pad.h"
#include "input/pad_map.h"

using namespace padmap;

static int g_checks = 0, g_failed = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failed;                                                   \
      std::printf("FEHLER %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

static bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }
constexpr float DT = 0.02f;

static void put16(uint8_t* d, uint16_t v) {
  d[0] = v & 0xFF;
  d[1] = v >> 8;
}

static void testReport() {
  uint8_t d[20] = {};
  put16(d + 0, 65535);  // links: ganz rechts
  put16(d + 2, 0);      //        ganz oben
  put16(d + 4, 32768);  // rechts: Mitte
  put16(d + 6, 65535);  //         ganz unten
  put16(d + 8, 1023);   // LT voll
  put16(d + 10, 0);
  d[12] = 2;     // Steuerkreuz oben rechts
  d[13] = 0x01 | 0x10 | 0x80;  // A, Y, RB
  d[14] = 0x08 | 0x10;         // Menü, Xbox
  d[15] = 0x01;                // Share
  pad::State s;
  CHECK(!pad::parseXboxReport(d, 15, s));
  CHECK(pad::parseXboxReport(d, 16, s));
  CHECK(near(s.lx, 1.0f) && near(s.ly, 1.0f) && near(s.rx, 0.0f) && near(s.ry, -1.0f));
  CHECK(near(s.lt, 1.0f) && near(s.rt, 0.0f));
  using namespace pad;
  const uint32_t want = mask(BTN_A) | mask(BTN_Y) | mask(BTN_RB) | mask(BTN_MENU) | mask(BTN_XBOX) | mask(BTN_SHARE) |
                        mask(BTN_UP) | mask(BTN_RIGHT);
  CHECK(s.buttons == want);
  // Elite 2: Paddles in Byte 18
  d[18] = 0x01 | 0x08;
  CHECK(parseXboxReport(d, 20, s));
  CHECK((s.buttons & mask(BTN_P1)) && (s.buttons & mask(BTN_P4)) && !(s.buttons & mask(BTN_P2)));
  // Steuerkreuz: alle Richtungen
  const uint32_t dirs[9] = {0, mask(BTN_UP), mask(BTN_UP) | mask(BTN_RIGHT), mask(BTN_RIGHT), mask(BTN_RIGHT) | mask(BTN_DOWN),
                            mask(BTN_DOWN), mask(BTN_DOWN) | mask(BTN_LEFT), mask(BTN_LEFT), mask(BTN_LEFT) | mask(BTN_UP)};
  const uint32_t dmask = mask(BTN_UP) | mask(BTN_DOWN) | mask(BTN_LEFT) | mask(BTN_RIGHT);
  for (int h = 0; h <= 8; ++h) {
    d[12] = h;
    parseXboxReport(d, 16, s);
    CHECK((s.buttons & dmask) == dirs[h]);
  }
  for (int i = 0; i < NUM_BUTTONS; ++i) CHECK(buttonFromName(buttonName(i)) == i);
  CHECK(buttonFromName("z") == -1);
}

static Output step(Mapper& m, const pad::State& s) { return m.update(s, DT); }
static pad::State press(int b);

static void testSticks() {
  Mapper m;
  pad::State s;
  // Totzone
  s.lx = 0.1f;
  Output o = step(m, s);
  CHECK(!o.walk_send && near(o.walk[1], 0.0f));
  // voll vor: vx = Grundtempo, mit RT voll 1, mit LT langsamer
  s = pad::State();
  s.ly = 1.0f;
  o = step(m, s);
  CHECK(o.walk_send && near(o.walk[0], 0.6f) && near(o.walk[1], 0.0f));
  s.rt = 1.0f;
  o = step(m, s);
  CHECK(near(o.walk[0], 1.0f));
  s.rt = 0.0f;
  s.lt = 1.0f;
  o = step(m, s);
  CHECK(o.walk[0] > 0.1f && o.walk[0] < 0.3f);
  // Richtungen: Stick rechts = nach rechts (vy < 0), rechter Stick rechts = rechts drehen
  s = pad::State();
  s.lx = 1.0f;
  s.rx = 1.0f;
  o = step(m, s);
  CHECK(o.walk[1] < -0.5f && o.walk[2] < -0.5f);
  // Kennlinie: halber Ausschlag gibt weniger als die Hälfte (fein um die Mitte)
  s = pad::State();
  s.ly = 0.5f;
  o = step(m, s);
  CHECK(o.walk[0] > 0.05f && o.walk[0] < 0.3f * 0.6f / 0.5f);
  // Loslassen: genau einmal null senden
  s = pad::State();
  o = step(m, s);
  CHECK(o.walk_send && near(o.walk[0], 0.0f));
  o = step(m, s);
  CHECK(!o.walk_send);
  // Y umkehren
  Config c = defaults();
  c.invert_y = true;
  m.setConfig(c);
  s.ly = 1.0f;
  o = step(m, s);
  CHECK(o.walk[0] < -0.5f);
  // Sticks tauschen: rechter Stick fährt, linker dreht
  c = defaults();
  c.swap_sticks = true;
  m.setConfig(c);
  s = pad::State();
  s.ry = 1.0f;
  o = step(m, s);
  CHECK(o.walk[0] > 0.5f && near(o.walk[2], 0.0f));
  s = pad::State();
  s.lx = 1.0f;
  o = step(m, s);
  CHECK(o.walk[2] < -0.5f && near(o.walk[0], 0.0f) && near(o.walk[1], 0.0f));
  // ... auch für die Vorderbeine (linker Fuß folgt dem rechten Stick)
  Mapper arms;
  arms.setConfig(c);
  step(arms, press(pad::BTN_Y));   // Laufen -> Körper
  step(arms, pad::State());
  step(arms, press(pad::BTN_Y));   // Körper -> Vorderbeine
  step(arms, pad::State());
  s = pad::State();
  s.rx = 1.0f;
  o = step(arms, s);
  CHECK(o.mode == Mode::Arms && o.arm[0] > 0.5f && near(o.arm[2], 0.0f));
}

static pad::State press(int b) {
  pad::State s;
  s.buttons = pad::mask(b);
  return s;
}

static void testButtons() {
  Mapper m;
  // A: einmal pro Druck, auch wenn gehalten
  Output o = step(m, press(pad::BTN_A));
  CHECK(o.n_actions == 1 && o.actions[0] == Action::StandSit);
  for (int i = 0; i < 50; ++i) {
    o = step(m, press(pad::BTN_A));
    CHECK(o.n_actions == 0);
  }
  step(m, pad::State());
  o = step(m, press(pad::BTN_A));
  CHECK(o.n_actions == 1);
  // Steuerkreuz oben (Höhe): wiederholt beim Halten
  m.reset();
  int n = 0;
  for (int i = 0; i < 50; ++i) n += step(m, press(pad::BTN_UP)).n_actions;  // 1 s
  CHECK(n >= 3 && n <= 5);
  step(m, pad::State());
  // Tempo: rechts schneller, links langsamer, begrenzt
  for (int i = 0; i < 20; ++i) {
    step(m, press(pad::BTN_RIGHT));
    step(m, pad::State());
  }
  CHECK(near(m.speed(), 1.0f));
  for (int i = 0; i < 20; ++i) {
    step(m, press(pad::BTN_LEFT));
    step(m, pad::State());
  }
  CHECK(near(m.speed(), 0.1f));
  // Modus: Y wechselt Laufen -> Körper -> Vorderbeine -> Laufen
  CHECK(m.mode() == Mode::Walk);
  o = step(m, press(pad::BTN_Y));
  CHECK(o.mode_changed && o.mode == Mode::Body);
  step(m, pad::State());
  o = step(m, press(pad::BTN_Y));
  CHECK(o.mode == Mode::Arms);
  step(m, pad::State());
  o = step(m, press(pad::BTN_Y));
  CHECK(o.mode == Mode::Walk);
  step(m, pad::State());
  // Paddles direkt
  o = step(m, press(pad::BTN_P2));
  CHECK(o.mode == Mode::Body && o.mode_changed);
  o = step(m, press(pad::BTN_P2));
  CHECK(!o.mode_changed);
  step(m, pad::State());
  // Belegung ändern: A = Not-Aus, Y = nichts
  Config c = defaults();
  c.map[pad::BTN_A] = Action::Estop;
  c.map[pad::BTN_Y] = Action::None;
  m.setConfig(c);
  o = step(m, press(pad::BTN_A));
  CHECK(o.n_actions == 1 && o.actions[0] == Action::Estop);
  step(m, pad::State());
  o = step(m, press(pad::BTN_Y));
  CHECK(o.n_actions == 0 && !o.mode_changed);
  // beim Verbinden gehaltene Taste (Xbox zum Einschalten) löst nichts aus
  Mapper e;
  e.reset(pad::mask(pad::BTN_XBOX));
  o = step(e, press(pad::BTN_XBOX));
  CHECK(o.n_actions == 0);
  step(e, pad::State());
  o = step(e, press(pad::BTN_XBOX));
  CHECK(o.n_actions == 1 && o.actions[0] == Action::Estop);
  // gehaltene Wiederholtaste (Steuerkreuz) beim Verbinden: keine Wiederholung, bis losgelassen
  Mapper h;
  h.reset(pad::mask(pad::BTN_UP));
  int rep = 0;
  for (int i = 0; i < 50; ++i) rep += step(h, press(pad::BTN_UP)).n_actions;  // 1 s gehalten
  CHECK(rep == 0);
  step(h, pad::State());
  CHECK(step(h, press(pad::BTN_UP)).n_actions == 1);
  // Speichern der Einstellungen ändert das mit dem Steuerkreuz gewählte Tempo nicht
  Mapper sp;
  for (int i = 0; i < 4; ++i) {
    step(sp, press(pad::BTN_RIGHT));
    step(sp, pad::State());
  }
  const float chosen = sp.speed();
  CHECK(chosen > defaults().speed + 0.1f);
  Config inv = defaults();
  inv.invert_y = true;
  sp.setConfig(inv);
  CHECK(near(sp.speed(), chosen));
  inv.speed = 0.3f;  // Grundtempo geändert -> gilt
  sp.setConfig(inv);
  CHECK(near(sp.speed(), 0.3f));
  // Namen
  for (int i = 0; i < NUM_ACTIONS; ++i) {
    Action a;
    CHECK(actionFromName(actionName(static_cast<Action>(i)), a) && static_cast<int>(a) == i);
  }
  Action a;
  CHECK(!actionFromName("jump", a));
  // Vorgabe: jede Taste belegt, Xbox-Taste = Not-Aus
  Config d = defaults();
  for (int i = 0; i < pad::NUM_BUTTONS; ++i) CHECK(d.map[i] != Action::None);
  CHECK(d.map[pad::BTN_XBOX] == Action::Estop);
}

static void testModes() {
  Mapper m;
  pad::State s;
  // Körper: Sticks verschieben/neigen, Trigger drehen; beim Wechsel einmal null fürs Laufen
  s.ly = 1.0f;
  Output o = step(m, s);
  CHECK(o.walk_send);
  s.buttons = pad::mask(pad::BTN_Y);
  o = step(m, s);
  CHECK(o.mode == Mode::Body && o.walk_send && near(o.walk[0], 0.0f));  // Laufen: null
  CHECK(o.body_send && near(o.body[0], 1.0f));
  s = pad::State();
  s.lx = 1.0f;
  s.rx = 1.0f;
  s.ry = 1.0f;
  s.lt = 1.0f;
  o = step(m, s);
  CHECK(!o.walk_send);
  CHECK(o.body[1] < -0.9f && o.body[2] > 0.5f && o.body[3] > 0.5f && o.body[4] > 0.9f);
  // Vorderbeine: Fuß folgt dem Stick, rechtes Bein gespiegelt; RT streckt aus
  s = pad::State();
  s.buttons = pad::mask(pad::BTN_Y);
  step(m, s);
  s = pad::State();
  s.lx = 1.0f;
  s.rx = 1.0f;
  s.rt = 1.0f;
  o = step(m, s);
  CHECK(o.mode == Mode::Arms && o.arm_send);
  CHECK(o.arm[0] > 0.9f && near(o.arm[1], 0.0f) && o.arm[2] < -0.9f && o.arm[4] > 0.9f);
  s = pad::State();
  s.ly = 1.0f;
  s.ry = -1.0f;
  o = step(m, s);
  CHECK(o.arm[1] > 0.9f && o.arm[3] < -0.9f);
  // diagonal: runde Totzone, Betrag höchstens 1
  s = pad::State();
  s.lx = 1.0f;
  s.ly = 1.0f;
  o = step(m, s);
  CHECK(std::hypot(o.arm[0], o.arm[1]) <= 1.0f + 1e-4f && near(o.arm[0], o.arm[1]));
}

int main() {
  testReport();
  testSticks();
  testButtons();
  testModes();
  std::printf("%d Prüfungen, %d Fehler\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
