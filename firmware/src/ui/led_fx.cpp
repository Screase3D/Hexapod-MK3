#include "led_fx.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace ledfx {
namespace {
constexpr float TWO_PI = 6.28318531f;

const char* const EFFECT_NAMES[NUM_EFFECTS] = {"off", "solid", "breathe", "blink", "pulse", "wink", "alternate", "rainbow"};
const char* const STATE_NAMES[NUM_STATES] = {"boot", "wifi", "ap", "idle", "torque", "stand",
                                             "walk", "arms", "gesture", "bat_warn", "bat_low", "estop"};
const char* const MODE_NAMES[3] = {"auto", "single", "off"};
const char* const BOARD_NAMES[3] = {"boot", "mirror", "off"};

template <typename T>
bool fromName(const char* name, const char* const* names, int n, T& out) {
  if (!name) return false;
  for (int i = 0; i < n; ++i) {
    if (std::strcmp(name, names[i]) == 0) {
      out = static_cast<T>(i);
      return true;
    }
  }
  return false;
}

Style style(uint8_t r, uint8_t g, uint8_t b, Effect e) {
  Style s;
  s.color = {r, g, b};
  s.effect = e;
  return s;
}

uint32_t hash32(uint32_t x) {  // für das zufällig wirkende Blinzeln (deterministisch)
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

// Lid zu: 0 in der Mitte eines 160-ms-Blinzelns, sonst 1
float lid(int32_t dt) {
  if (dt < 0 || dt > 160) return 1.0f;
  float k = std::fabs(dt - 80.0f) / 80.0f;
  return k;
}

Rgb hsv(float h_deg) {
  h_deg = std::fmod(h_deg, 360.0f);
  if (h_deg < 0) h_deg += 360.0f;
  const float h = h_deg / 60.0f;
  const int i = static_cast<int>(h);
  const float f = h - i;
  const uint8_t q = static_cast<uint8_t>(lroundf(255.0f * (1.0f - f)));
  const uint8_t t = static_cast<uint8_t>(lroundf(255.0f * f));
  switch (i % 6) {
    case 0: return {255, t, 0};
    case 1: return {q, 255, 0};
    case 2: return {0, 255, t};
    case 3: return {0, q, 255};
    case 4: return {t, 0, 255};
    default: return {255, 0, q};
  }
}
}  // namespace

bool operator==(const Rgb& a, const Rgb& b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

const char* effectName(Effect e) {
  int i = static_cast<int>(e);
  return i < NUM_EFFECTS ? EFFECT_NAMES[i] : "off";
}
bool effectFromName(const char* name, Effect& out) { return fromName(name, EFFECT_NAMES, NUM_EFFECTS, out); }

const char* stateName(State s) {
  int i = static_cast<int>(s);
  return i < NUM_STATES ? STATE_NAMES[i] : "idle";
}
bool stateFromName(const char* name, State& out) { return fromName(name, STATE_NAMES, NUM_STATES, out); }

bool configurable(State s) { return s >= State::Ap && s <= State::Gesture; }

const char* modeName(Mode m) { return MODE_NAMES[static_cast<int>(m) % 3]; }
bool modeFromName(const char* name, Mode& out) { return fromName(name, MODE_NAMES, 3, out); }
const char* boardName(Board b) { return BOARD_NAMES[static_cast<int>(b) % 3]; }
bool boardFromName(const char* name, Board& out) { return fromName(name, BOARD_NAMES, 3, out); }

Style fixedStyle(State s) {
  switch (s) {
    case State::Boot:        return style(102, 102, 102, Effect::Solid);
    case State::WifiConnect: return style(0, 0, 255, Effect::Blink);
    case State::BatteryWarn: return style(255, 110, 0, Effect::Solid);
    case State::BatteryLow:  return style(255, 0, 0, Effect::Breathe);
    case State::Estop:       return style(255, 0, 0, Effect::Blink);
    default:                 return style(0, 0, 0, Effect::Off);
  }
}

Config defaults() {
  Config c;
  c.single = style(0, 255, 255, Effect::Wink);
  for (int i = 0; i < NUM_STATES; ++i) c.styles[i] = fixedStyle(static_cast<State>(i));
  c.styles[static_cast<int>(State::Ap)] = style(0, 80, 255, Effect::Breathe);
  c.styles[static_cast<int>(State::Idle)] = style(0, 153, 0, Effect::Solid);
  c.styles[static_cast<int>(State::Torque)] = style(0, 255, 255, Effect::Solid);
  c.styles[static_cast<int>(State::Stand)] = style(0, 255, 255, Effect::Wink);
  c.styles[static_cast<int>(State::Walk)] = style(0, 140, 255, Effect::Solid);
  c.styles[static_cast<int>(State::Arms)] = style(170, 0, 255, Effect::Breathe);
  c.styles[static_cast<int>(State::Gesture)] = style(255, 0, 160, Effect::Rainbow);
  return c;
}

Style styleFor(const Config& c, State s) {
  if (!configurable(s)) return fixedStyle(s);
  switch (c.mode) {
    case Mode::Off:    return style(0, 0, 0, Effect::Off);
    case Mode::Single: return c.single;
    default:           return c.styles[static_cast<int>(s)];
  }
}

void render(const Style& s, uint32_t t, Rgb out[2]) {
  float k[2] = {1.0f, 1.0f};
  switch (s.effect) {
    case Effect::Off:
      k[0] = k[1] = 0.0f;
      break;
    case Effect::Solid:
      break;
    case Effect::Breathe:
      k[0] = k[1] = 0.12f + 0.88f * (0.5f - 0.5f * std::cos(TWO_PI * (t % 3200) / 3200.0f));
      break;
    case Effect::Blink:
      k[0] = k[1] = (t % 1000) < 500 ? 1.0f : 0.0f;
      break;
    case Effect::Pulse: {  // Herzschlag: zwei Schläge, dann Pause
      const float x = static_cast<float>(t % 1400);
      const float b1 = std::exp(-((x - 100.0f) / 60.0f) * ((x - 100.0f) / 60.0f));
      const float b2 = 0.6f * std::exp(-((x - 350.0f) / 60.0f) * ((x - 350.0f) / 60.0f));
      k[0] = k[1] = 0.12f + 0.88f * (b1 > b2 ? b1 : b2);
      break;
    }
    case Effect::Wink: {  // an, alle 4 s an zufälliger Stelle kurz blinzeln (manchmal doppelt)
      const uint32_t n = t / 4000;
      const uint32_t h = hash32(n + 1);
      const int32_t start = 500 + static_cast<int32_t>(h % 3000);
      const int32_t dt = static_cast<int32_t>(t % 4000) - start;
      float v = lid(dt);
      if ((h >> 12) % 4 == 0) v = v < lid(dt - 300) ? v : lid(dt - 300);
      k[0] = k[1] = v;
      break;
    }
    case Effect::Alternate:
      k[0] = (t % 1000) < 500 ? 1.0f : 0.0f;
      k[1] = 1.0f - k[0];
      break;
    case Effect::Rainbow: {
      const float h = (t % 7000) * (360.0f / 7000.0f);
      out[0] = hsv(h);
      out[1] = hsv(h + 40.0f);
      return;
    }
  }
  out[0] = scale(s.color, k[0]);
  out[1] = scale(s.color, k[1]);
}

Rgb scale(const Rgb& c, float k) {
  if (k <= 0.0f) return {};
  if (k > 1.0f) k = 1.0f;
  return {static_cast<uint8_t>(lroundf(c.r * k)), static_cast<uint8_t>(lroundf(c.g * k)),
          static_cast<uint8_t>(lroundf(c.b * k))};
}

bool parseHex(const char* s, Rgb& out) {
  if (!s || s[0] != '#' || std::strlen(s) != 7) return false;
  unsigned v = 0;
  for (int i = 1; i < 7; ++i) {
    const char c = s[i];
    unsigned d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return false;
    v = v << 4 | d;
  }
  out = {static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8 & 0xFF), static_cast<uint8_t>(v & 0xFF)};
  return true;
}

void toHex(const Rgb& c, char out[8]) { std::snprintf(out, 8, "#%02x%02x%02x", c.r, c.g, c.b); }

}  // namespace ledfx
