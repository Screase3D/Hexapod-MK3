// Tests für die Augen-LEDs (Zustände, Stile, Effekte):
//   g++ -std=c++17 -Wall -Wextra -Werror -I../src -I../include led_test.cpp ../src/ui/led_fx.cpp -o led_test && ./led_test

#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "ui/led_fx.h"

using namespace ledfx;

static int g_checks = 0, g_failed = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failed;                                                   \
      std::printf("FEHLER %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

static int level(const Rgb& c) { return c.r + c.g + c.b; }

static void testNames() {
  for (int i = 0; i < NUM_EFFECTS; ++i) {
    Effect e;
    CHECK(effectFromName(effectName(static_cast<Effect>(i)), e) && static_cast<int>(e) == i);
  }
  for (int i = 0; i < NUM_STATES; ++i) {
    State s;
    CHECK(stateFromName(stateName(static_cast<State>(i)), s) && static_cast<int>(s) == i);
  }
  Mode m;
  CHECK(modeFromName("single", m) && m == Mode::Single);
  Board b;
  CHECK(boardFromName("mirror", b) && b == Board::Mirror);
  Effect e;
  CHECK(!effectFromName("disco", e));
  CHECK(!effectFromName(nullptr, e));
  Rgb c;
  CHECK(parseHex("#12aBef", c) && c.r == 0x12 && c.g == 0xab && c.b == 0xef);
  CHECK(!parseHex("12abef", c) && !parseHex("#12abeg", c) && !parseHex("#12abe", c));
  char hex[8];
  toHex({1, 2, 255}, hex);
  CHECK(std::strcmp(hex, "#0102ff") == 0);
}

static void testStyles() {
  Config c = defaults();
  // Warnungen sind fest, auch bei "aus" und "eine Farbe"
  for (Mode m : {Mode::Auto, Mode::Single, Mode::Off}) {
    c.mode = m;
    CHECK(styleFor(c, State::Estop).color == (Rgb{255, 0, 0}));
    CHECK(styleFor(c, State::BatteryLow).effect == Effect::Breathe);
    CHECK(styleFor(c, State::WifiConnect).effect == Effect::Blink);
  }
  c.mode = Mode::Off;
  CHECK(styleFor(c, State::Walk).effect == Effect::Off);
  c.mode = Mode::Single;
  c.single = {{10, 20, 30}, Effect::Pulse};
  CHECK(styleFor(c, State::Stand).color == (Rgb{10, 20, 30}) && styleFor(c, State::Idle).effect == Effect::Pulse);
  c.mode = Mode::Auto;
  CHECK(styleFor(c, State::Gesture).effect == Effect::Rainbow);
  CHECK(styleFor(c, State::Stand).effect == Effect::Wink);
  // jeder einstellbare Zustand hat eine sichtbare Vorgabe
  for (int i = 0; i < NUM_STATES; ++i) {
    State s = static_cast<State>(i);
    CHECK(configurable(s) == (s >= State::Ap && s <= State::Gesture));
    CHECK(styleFor(defaults(), s).effect != Effect::Off);
  }
}

static void testEffects() {
  const Rgb col{200, 100, 50};
  Rgb o[2];
  render({col, Effect::Off}, 1234, o);
  CHECK(level(o[0]) == 0 && level(o[1]) == 0);
  render({col, Effect::Solid}, 1234, o);
  CHECK(o[0] == col && o[1] == col);

  // Atmen: zwischen ca. 12 % und 100 %, nie aus
  int lo = 1000, hi = 0;
  for (uint32_t t = 0; t < 6400; t += 10) {
    render({col, Effect::Breathe}, t, o);
    lo = level(o[0]) < lo ? level(o[0]) : lo;
    hi = level(o[0]) > hi ? level(o[0]) : hi;
    CHECK(o[0] == o[1]);
  }
  CHECK(lo > 0 && lo < 60 && hi == level(col));

  // Blinken: halbe Zeit an
  int on = 0, n = 0;
  for (uint32_t t = 0; t < 10000; t += 10, ++n) {
    render({col, Effect::Blink}, t, o);
    on += level(o[0]) > 0;
  }
  CHECK(on * 2 == n);

  // Wechselblinken: immer genau ein Auge an
  for (uint32_t t = 0; t < 3000; t += 10) {
    render({col, Effect::Alternate}, t, o);
    CHECK((level(o[0]) > 0) != (level(o[1]) > 0));
  }

  // Blinzeln: fast immer an, je 4 s einmal (manchmal doppelt) kurz zu
  int closed = 0, blinks = 0;
  bool was_closed = false;
  for (uint32_t t = 0; t < 400000; t += 5) {
    render({col, Effect::Wink}, t, o);
    bool c = level(o[0]) < level(col) / 4;
    closed += c;
    if (c && !was_closed) ++blinks;
    was_closed = c;
  }
  std::printf("Info: Blinzeln in 400 s: %d mal, %.1f %% der Zeit zu\n", blinks, closed * 5 / 4000.0);
  CHECK(blinks >= 100 && blinks <= 200);
  CHECK(closed * 5 < 400000 / 50);  // unter 2 % der Zeit

  // Herzschlag: zwei Spitzen je 1,4 s
  int peaks = 0;
  int prev2 = 0, prev1 = 0;
  for (uint32_t t = 0; t < 1400; t += 5) {
    render({col, Effect::Pulse}, t, o);
    int v = level(o[0]);
    if (prev1 > prev2 && prev1 > v && prev1 > 150) ++peaks;
    prev2 = prev1;
    prev1 = v;
  }
  CHECK(peaks == 2);

  // Regenbogen: Farbe wandert, Augen versetzt, volle Sättigung
  Rgb a[2], b[2];
  render({col, Effect::Rainbow}, 0, a);
  render({col, Effect::Rainbow}, 2000, b);
  CHECK(!(a[0] == b[0]));
  CHECK(!(a[0] == a[1]));
  for (uint32_t t = 0; t < 7000; t += 50) {
    render({col, Effect::Rainbow}, t, o);
    int mx = o[0].r > o[0].g ? o[0].r : o[0].g;
    mx = mx > o[0].b ? mx : o[0].b;
    CHECK(mx == 255);
  }
  CHECK(scale(col, 0.5f) == (Rgb{100, 50, 25}));
  CHECK(scale(col, 2.0f) == col);
}

int main() {
  testNames();
  testStyles();
  testEffects();
  std::printf("%d Prüfungen, %d Fehler\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
