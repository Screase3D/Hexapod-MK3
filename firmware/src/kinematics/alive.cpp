#include "alive.h"

#include <cmath>

#include "gait.h"

namespace alive {
namespace {
constexpr float TWO_PI = 6.28318531f;
constexpr float DEG2RAD = 0.0174532925f;
constexpr float FADE_S = 0.4f;       // Ein-/Ausblenden
constexpr float BREATH_MM = 1.2f;    // Atmen: Körper hebt und senkt sich ...
constexpr float BREATH_S = 4.5f;     // ... so langsam
constexpr float PAUSE_MIN = 2.5f;    // Pause zwischen zwei Aktionen
constexpr float PAUSE_MAX = 7.0f;

// Häufigkeit je Aktion (Index = Action)
constexpr uint8_t WEIGHT[NUM_ACTIONS] = {0, 28, 18, 14, 16, 12, 12, 0, 0, 0};

float approach(float cur, float target, float step) {
  float d = target - cur;
  if (d > step) return cur + step;
  if (d < -step) return cur - step;
  return target;
}

gestures::Offset lerp(const gestures::Offset& a, const gestures::Offset& b, float s) {
  gestures::Offset o;
  o.x = a.x + (b.x - a.x) * s;
  o.y = a.y + (b.y - a.y) * s;
  o.z = a.z + (b.z - a.z) * s;
  o.roll = a.roll + (b.roll - a.roll) * s;
  o.pitch = a.pitch + (b.pitch - a.pitch) * s;
  o.yaw = a.yaw + (b.yaw - a.yaw) * s;
  return o;
}
}  // namespace

const char* actionName(Action a) {
  switch (a) {
    case Action::Look:    return "look";
    case Action::Shift:   return "shift";
    case Action::Tilt:    return "tilt";
    case Action::Lift:    return "lift";
    case Action::Tap:     return "tap";
    case Action::Shuffle: return "shuffle";
    case Action::Yawn:    return "yawn";
    case Action::Sigh:    return "sigh";
    case Action::Stretch: return "stretch";
    default:              return "none";
  }
}

Animator::Animator(const kin::LegMount* mounts, uint32_t seed) : mounts_(mounts), rng_(seed ? seed : 1u) {
  pause_ = rnd(1.0f, 2.0f);
}

float Animator::rnd() {
  // xorshift32
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return (rng_ >> 8) * (1.0f / 16777216.0f);
}

float Animator::rnd(float a, float b) { return a + (b - a) * rnd(); }

Animator::Key& Animator::add(float move_s, float hold_s) {
  Key& k = keys_[nkeys_ < MAX_KEYS ? nkeys_++ : MAX_KEYS - 1];
  k = Key();
  k.move_s = move_s;
  k.hold_s = hold_s;
  return k;
}

gestures::Offset Animator::away(int leg, float mm) const {
  // Richtung Körpermitte -> Fuß (Coxa plus Montagerichtung), Körper in die Gegenrichtung
  const kin::LegMount& m = mounts_[leg];
  const float a = m.angle_deg * DEG2RAD;
  const float fx = m.x + 100.0f * std::cos(a), fy = m.y + 100.0f * std::sin(a);
  const float len = std::sqrt(fx * fx + fy * fy);
  gestures::Offset o;
  o.x = -fx / len * mm;
  o.y = -fy / len * mm;
  return o;
}

void Animator::reset() {
  act_ = Action::None;
  forced_ = false;
  level_ = 0.0f;
  pause_ = rnd(1.0f, 2.0f);
}

void Animator::start(Action a) {
  plan(a);
  pause_ = 0.0f;
}

void Animator::play(Action a) {
  if (a == Action::None) return;
  plan(a);
  forced_ = true;
}

void Animator::plan(Action a) {
  nkeys_ = 0;
  add(0.0f, 0.0f);  // Ausgangspunkt: null
  const float side = rnd() < 0.5f ? -1.0f : 1.0f;
  switch (a) {
    case Action::Look: {  // zur Seite schauen, manchmal noch zur anderen
      Key& k = add(rnd(0.6f, 0.9f), rnd(0.6f, 1.6f));
      k.body.yaw = side * rnd(7.0f, 13.0f);
      k.body.y = side * rnd(2.0f, 5.0f);
      k.body.pitch = rnd(-1.0f, 3.0f);
      if (rnd() < 0.5f) {
        Key& o = add(rnd(1.1f, 1.5f), rnd(0.5f, 1.3f));
        o.body.yaw = -keys_[1].body.yaw * rnd(0.7f, 1.0f);
        o.body.y = -keys_[1].body.y * 0.8f;
        o.body.pitch = rnd(-1.0f, 3.0f);
      }
      add(rnd(0.7f, 1.0f), 0.0f);
      break;
    }
    case Action::Shift: {  // Gewicht verlagern, als würde er umstehen
      const float dir = rnd(0.0f, TWO_PI), d = rnd(6.0f, 12.0f);
      Key& k = add(rnd(1.0f, 1.6f), rnd(1.0f, 3.0f));
      k.body.x = d * std::cos(dir);
      k.body.y = d * std::sin(dir);
      k.body.z = -rnd(0.0f, 3.0f);
      add(rnd(1.0f, 1.6f), 0.0f);
      break;
    }
    case Action::Tilt: {  // neugierig: Kopf schief, Nase etwas hoch
      Key& k = add(0.6f, rnd(0.8f, 1.8f));
      k.body.roll = side * rnd(3.0f, 6.0f);
      k.body.pitch = rnd(2.0f, 5.0f);
      k.body.yaw = -side * rnd(0.0f, 4.0f);
      k.body.z = rnd(0.0f, 4.0f);
      add(0.7f, 0.0f);
      break;
    }
    case Action::Lift: {  // ein Bein kurz anheben (Gewicht vorher weg davon)
      const int leg = static_cast<int>(rnd() * 6.0f) % 6;
      const gestures::Offset w = away(leg, 5.0f);
      add(0.5f, 0.0f).body = w;
      Key& up = add(0.3f, rnd(0.2f, 0.6f));
      up.body = w;
      up.leg = static_cast<int8_t>(leg);
      up.foot = {0.0f, 0.0f, rnd(14.0f, 22.0f)};
      add(0.3f, 0.1f).body = w;
      add(0.5f, 0.0f);
      break;
    }
    case Action::Tap: {  // ungeduldig mit einem Vorderbein tippen
      const int leg = side < 0.0f ? 0 : 3;
      const gestures::Offset w = away(leg, 4.0f);
      add(0.4f, 0.0f).body = w;
      const int taps = rnd() < 0.5f ? 2 : 3;
      for (int i = 0; i < taps; ++i) {
        Key& up = add(0.15f, 0.05f);
        up.body = w;
        up.leg = static_cast<int8_t>(leg);
        up.foot = {4.0f, 0.0f, rnd(10.0f, 14.0f)};
        add(0.15f, 0.12f).body = w;
      }
      add(0.4f, 0.0f);
      break;
    }
    case Action::Shuffle: {  // Füße einmal neu setzen: ein Bein nach dem anderen
      static constexpr int ORDER[6] = {0, 4, 2, 3, 1, 5};
      const bool rev = rnd() < 0.5f;
      for (int i = 0; i < 6; ++i) {
        const int leg = ORDER[rev ? 5 - i : i];
        Key& up = add(0.2f, 0.02f);
        up.body = away(leg, 2.5f);
        up.leg = static_cast<int8_t>(leg);
        up.foot = {0.0f, 0.0f, rnd(10.0f, 13.0f)};
        add(0.2f, 0.06f);
      }
      add(0.3f, 0.0f);
      break;
    }
    case Action::Yawn: {  // gähnen: Kopf hoch, Körper streckt sich, dann zusammensacken
      Key& k = add(1.2f, rnd(0.8f, 1.3f));
      k.body.pitch = rnd(7.0f, 10.0f);
      k.body.z = rnd(3.0f, 5.0f);
      k.body.x = -2.0f;
      k.body.roll = side * rnd(0.0f, 2.0f);
      Key& s = add(0.9f, 0.4f);
      s.body.pitch = -3.0f;
      s.body.z = -3.0f;
      add(1.0f, 0.0f);
      break;
    }
    case Action::Sigh: {  // seufzen: langsam ausatmen und zusammensinken
      Key& k = add(0.8f, 0.2f);
      k.body.z = 2.0f;
      k.body.pitch = 2.0f;
      Key& s = add(1.6f, rnd(0.4f, 0.9f));
      s.body.z = -rnd(5.0f, 7.0f);
      s.body.pitch = -3.0f;
      add(1.4f, 0.0f);
      break;
    }
    case Action::Stretch: {  // strecken nach dem Aufwachen: hoch, nach hinten, nach vorne
      Key& up = add(1.0f, 0.7f);
      up.body.z = rnd(8.0f, 11.0f);
      up.body.pitch = 6.0f;
      Key& back = add(0.9f, 0.6f);
      back.body.x = -8.0f;
      back.body.pitch = -5.0f;
      back.body.z = 2.0f;
      Key& fwd = add(0.9f, 0.4f);
      fwd.body.x = 6.0f;
      fwd.body.pitch = 3.0f;
      add(0.8f, 0.0f);
      break;
    }
    default:
      break;
  }
  act_ = a;
  key_ = 1;
  key_t_ = 0.0f;
}

Output Animator::sample() const {
  Output o;
  if (act_ == Action::None || key_ < 1 || key_ >= nkeys_) return o;
  const Key& a = keys_[key_ - 1];
  const Key& b = keys_[key_];
  const float s = b.move_s > 0.0f ? gait::smoothStep5(key_t_ / b.move_s) : 1.0f;
  o.body = lerp(a.body, b.body, s);
  for (int leg = 0; leg < 6; ++leg) {
    const kin::Vec3 fa = a.leg == leg ? a.foot : kin::Vec3{0, 0, 0};
    const kin::Vec3 fb = b.leg == leg ? b.foot : kin::Vec3{0, 0, 0};
    o.foot[leg] = {fa.x + (fb.x - fa.x) * s, fa.y + (fb.y - fa.y) * s, fa.z + (fb.z - fa.z) * s};
  }
  return o;
}

Output Animator::update(float dt, bool enabled, bool allowed) {
  const bool on = (enabled || forced_) && allowed;
  level_ = approach(level_, on ? 1.0f : 0.0f, dt / FADE_S);
  breath_t_ += dt;
  if (breath_t_ > BREATH_S) breath_t_ -= BREATH_S;

  if (!on && level_ <= 0.0f) {
    // ganz aus: Aktion abbrechen; danach erst nach einer Weile wieder (beim
    // Einschalten bald, nach Laufen o. Ä. etwas später)
    act_ = Action::None;
    forced_ = false;
    pause_ = enabled ? rnd(PAUSE_MIN, 4.5f) : rnd(1.0f, 2.0f);
    return Output();
  }

  if (act_ == Action::None) {
    if (on && level_ >= 1.0f) {
      pause_ -= dt;
      if (pause_ <= 0.0f) {
        // zufällige Aktion, nicht zweimal dieselbe hintereinander
        int total = 0;
        for (int i = 1; i < NUM_ACTIONS; ++i) total += i == static_cast<int>(last_) ? 0 : WEIGHT[i];
        float pick = rnd() * total;
        Action a = Action::Look;
        for (int i = 1; i < NUM_ACTIONS; ++i) {
          if (i == static_cast<int>(last_)) continue;
          pick -= WEIGHT[i];
          if (pick < 0.0f) {
            a = static_cast<Action>(i);
            break;
          }
        }
        plan(a);
      }
    }
  } else {
    key_t_ += dt;
    const Key& k = keys_[key_];
    if (key_t_ >= k.move_s + k.hold_s) {
      key_t_ -= k.move_s + k.hold_s;
      if (++key_ >= nkeys_) {
        last_ = act_;
        act_ = Action::None;
        forced_ = false;
        pause_ = rnd(PAUSE_MIN, PAUSE_MAX);
      }
    }
  }

  Output o = sample();
  if (breath_on_) o.body.z += BREATH_MM * std::sin(TWO_PI * breath_t_ / BREATH_S);
  const float g = gait::smoothStep5(level_);
  o.body.x *= g;
  o.body.y *= g;
  o.body.z *= g;
  o.body.roll *= g;
  o.body.pitch *= g;
  o.body.yaw *= g;
  for (auto& f : o.foot) f = {f.x * g, f.y * g, f.z * g};
  return o;
}

}  // namespace alive
