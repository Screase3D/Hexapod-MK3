#include "pad_map.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace padmap {
namespace {
const char* const ACTION_NAMES[NUM_ACTIONS] = {
    "none",      "stand_sit",   "stop",       "arms",      "wave",      "mode_next", "mode_walk",
    "mode_body", "mode_arms",   "gait_prev",  "gait_next", "height_up", "height_down", "speed_up",
    "speed_down", "alive",      "leveling",   "gesture_next", "circle", "nod",       "shake",
    "pushup",    "dance",       "torque",     "estop",     "pose_reset"};

constexpr float REPEAT_DELAY_S = 0.45f;  // Halten: erste Wiederholung nach ...
constexpr float REPEAT_S = 0.2f;         // ... dann alle
constexpr float SPEED_STEP = 0.1f;

bool repeats(Action a) {
  return a == Action::HeightUp || a == Action::HeightDown || a == Action::SpeedUp || a == Action::SpeedDown;
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
bool any(const float* v, int n) {
  for (int i = 0; i < n; ++i) {
    if (std::fabs(v[i]) > 1e-3f) return true;
  }
  return false;
}
}  // namespace

const char* actionName(Action a) {
  int i = static_cast<int>(a);
  return i < NUM_ACTIONS ? ACTION_NAMES[i] : "none";
}

bool actionFromName(const char* name, Action& out) {
  if (!name) return false;
  for (int i = 0; i < NUM_ACTIONS; ++i) {
    if (std::strcmp(name, ACTION_NAMES[i]) == 0) {
      out = static_cast<Action>(i);
      return true;
    }
  }
  return false;
}

const char* modeName(Mode m) {
  switch (m) {
    case Mode::Body: return "body";
    case Mode::Arms: return "arms";
    default:         return "walk";
  }
}

Config defaults() {
  Config c;
  c.map[pad::BTN_A] = Action::StandSit;
  c.map[pad::BTN_B] = Action::Stop;
  c.map[pad::BTN_X] = Action::ArmsToggle;
  c.map[pad::BTN_Y] = Action::ModeNext;
  c.map[pad::BTN_LB] = Action::GaitPrev;
  c.map[pad::BTN_RB] = Action::GaitNext;
  c.map[pad::BTN_VIEW] = Action::AliveToggle;
  c.map[pad::BTN_MENU] = Action::GestureNext;
  c.map[pad::BTN_XBOX] = Action::Estop;
  c.map[pad::BTN_LS] = Action::LevelToggle;
  c.map[pad::BTN_RS] = Action::Wave;
  c.map[pad::BTN_UP] = Action::HeightUp;
  c.map[pad::BTN_DOWN] = Action::HeightDown;
  c.map[pad::BTN_LEFT] = Action::SpeedDown;
  c.map[pad::BTN_RIGHT] = Action::SpeedUp;
  c.map[pad::BTN_SHARE] = Action::PoseReset;
  c.map[pad::BTN_P1] = Action::ModeWalk;   // Elite-Paddles
  c.map[pad::BTN_P2] = Action::ModeBody;
  c.map[pad::BTN_P3] = Action::ModeArms;
  c.map[pad::BTN_P4] = Action::Stop;
  return c;
}

void Mapper::setConfig(const Config& c) {
  const float before = cfg_.speed;
  cfg_ = c;
  cfg_.deadzone = clampf(cfg_.deadzone, 0.02f, 0.4f);
  cfg_.expo = clampf(cfg_.expo, 0.0f, 1.0f);
  cfg_.speed = clampf(cfg_.speed, 0.1f, 1.0f);
  // mit dem Steuerkreuz gewähltes Tempo nur zurücksetzen, wenn sich das Grundtempo ändert
  // (nicht bei jedem Speichern der Controller-Einstellungen)
  if (cfg_.speed != before) speed_ = cfg_.speed;
}

void Mapper::reset(uint32_t held) {
  prev_ = held;
  for (int i = 0; i < pad::NUM_BUTTONS; ++i) {
    held_[i] = 0.0f;
    // beim Verbinden gehaltene Tasten: auch keine Wiederholung, bis sie losgelassen werden
    next_repeat_[i] = (held & pad::mask(i)) ? 1e9f : 0.0f;
  }
}

float Mapper::axis(float v) const {
  if (!std::isfinite(v)) return 0.0f;  // z. B. "1e999" aus dem Browser
  const float a = std::fabs(v);
  if (a <= cfg_.deadzone) return 0.0f;
  float s = clampf((a - cfg_.deadzone) / (1.0f - cfg_.deadzone), 0.0f, 1.0f);
  s = (1.0f - cfg_.expo) * s + cfg_.expo * s * s * s;
  return v < 0.0f ? -s : s;
}

void Mapper::stick(float x, float y, float& ox, float& oy) const {
  // runde Totzone, Betrag neu skaliert; Kennlinie je Achse
  const float m = std::sqrt(x * x + y * y);
  if (!std::isfinite(m) || m <= cfg_.deadzone) {  // unendlich: inf * 0 ergäbe NaN
    ox = oy = 0.0f;
    return;
  }
  const float k = clampf((m - cfg_.deadzone) / (1.0f - cfg_.deadzone), 0.0f, 1.0f) / m;
  auto curve = [&](float v) {
    v = clampf(v * k, -1.0f, 1.0f);
    const float a = std::fabs(v);
    const float s = (1.0f - cfg_.expo) * a + cfg_.expo * a * a * a;
    return v < 0.0f ? -s : s;
  };
  ox = curve(x);
  oy = curve(y);
}

Output Mapper::update(const pad::State& in, float dt) {
  Output o;
  // Tasten: Druck (Flanke) und Wiederholung beim Halten
  const uint32_t pressed = in.buttons & ~prev_;
  for (int b = 0; b < pad::NUM_BUTTONS; ++b) {
    const Action a = cfg_.map[b];
    if (!(in.buttons & pad::mask(b))) {
      held_[b] = 0.0f;
      continue;
    }
    bool fire = pressed & pad::mask(b);
    if (fire) {
      held_[b] = 0.0f;
      next_repeat_[b] = REPEAT_DELAY_S;
    } else {
      held_[b] += dt;
      if (repeats(a) && held_[b] >= next_repeat_[b]) {
        fire = true;
        next_repeat_[b] += REPEAT_S;
      }
    }
    if (!fire || a == Action::None || o.n_actions >= Output::MAX_ACTIONS) continue;
    o.actions[o.n_actions++] = a;
    // Modus und Tempo verwaltet der Mapper selbst
    const Mode before = mode_;
    switch (a) {
      case Action::ModeNext: mode_ = static_cast<Mode>((static_cast<int>(mode_) + 1) % 3); break;
      case Action::ModeWalk: mode_ = Mode::Walk; break;
      case Action::ModeBody: mode_ = Mode::Body; break;
      case Action::ModeArms: mode_ = Mode::Arms; break;
      case Action::SpeedUp: speed_ = clampf(speed_ + SPEED_STEP, 0.1f, 1.0f); break;
      case Action::SpeedDown: speed_ = clampf(speed_ - SPEED_STEP, 0.1f, 1.0f); break;
      default: break;
    }
    if (mode_ != before) o.mode_changed = true;
  }
  prev_ = in.buttons;
  o.mode = mode_;

  // Sticks und Trigger
  float lx, ly, rx, ry;
  const float inv = cfg_.invert_y ? -1.0f : 1.0f;
  stick(in.lx, in.ly * inv, lx, ly);
  stick(in.rx, in.ry * inv, rx, ry);
  if (cfg_.swap_sticks) {
    std::swap(lx, rx);
    std::swap(ly, ry);
  }
  const float lt = axis(in.lt), rt = axis(in.rt);
  switch (mode_) {
    case Mode::Walk: {
      // Tempo: Grundtempo, RT gibt Gas bis 100 %, LT bremst
      const float s = clampf(speed_ * (1.0f - 0.6f * lt) + (1.0f - speed_) * rt, 0.05f, 1.0f);
      o.walk[0] = ly * s;
      o.walk[1] = -lx * s;   // Stick rechts = nach rechts (y links +)
      o.walk[2] = -rx * s;   // Stick rechts = rechts herum drehen
      break;
    }
    case Mode::Body:
      o.body[0] = ly;        // vor/zurück
      o.body[1] = -lx;       // seitlich
      o.body[2] = rx;        // rechts runter
      o.body[3] = ry;        // Nase hoch
      o.body[4] = lt - rt;   // LT links, RT rechts drehen
      break;
    case Mode::Arms:
      // Fuß folgt dem Stick (von hinten gesehen); Firmware: x + = nach innen
      o.arm[0] = lx;
      o.arm[1] = ly;
      o.arm[2] = -rx;
      o.arm[3] = ry;
      o.arm[4] = rt - lt;    // RT ausstrecken, LT heranziehen
      break;
  }
  const bool walk_now = any(o.walk, 3), body_now = any(o.body, 5), arm_now = any(o.arm, 5);
  o.walk_send = walk_now || walk_was_;
  o.body_send = body_now || body_was_;
  o.arm_send = arm_now || arm_was_;
  walk_was_ = walk_now;
  body_was_ = body_now;
  arm_was_ = arm_now;
  return o;
}

}  // namespace padmap
