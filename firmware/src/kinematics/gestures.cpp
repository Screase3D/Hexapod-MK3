#include "gestures.h"

#include <cmath>
#include <cstring>

#include "gait.h"

namespace gestures {
namespace {
constexpr float TWO_PI = 6.28318531f;
constexpr float FADE_S = 0.8f;  // Ein- und Ausblenden

// Dauer je Geste (s), Index = Type
constexpr float DURATION[] = {0.0f, 6.0f, 3.4f, 3.4f, 5.6f, 6.4f};
}  // namespace

const char* typeName(Type t) {
  switch (t) {
    case Type::Circle: return "circle";
    case Type::Nod:    return "nod";
    case Type::Shake:  return "shake";
    case Type::Pushup: return "pushup";
    case Type::Dance:  return "dance";
    default:           return "none";
  }
}

bool typeFromName(const char* name, Type& out) {
  if (!name) return false;
  for (int i = 1; i <= static_cast<int>(Type::Dance); ++i) {
    if (std::strcmp(name, typeName(static_cast<Type>(i))) == 0) {
      out = static_cast<Type>(i);
      return true;
    }
  }
  return false;
}

float Player::duration() const { return DURATION[static_cast<int>(type_)]; }

void Player::start(Type t) {
  type_ = t;
  t_ = 0.0f;
  stop_at_ = -1.0f;
}

void Player::stop() {
  if (type_ == Type::None || stop_at_ >= 0.0f) return;
  // vom aktuellen Pegel aus ausblenden (ohne Sprung)
  float d = duration();
  float in = t_ / FADE_S;
  float level = in < 1.0f ? gait::smoothStep5(in) : 1.0f;
  if (d - t_ < FADE_S) return;  // blendet ohnehin gleich aus
  // Ausblend-Hüllkurve smoothStep5(v) so legen, dass sie beim aktuellen Pegel
  // beginnt: v suchen mit smoothStep5(v) = level, dann stop_at = t - (1 - v) * FADE_S
  float lo = 0.0f, hi = 1.0f;
  for (int i = 0; i < 24; ++i) {
    float mid = 0.5f * (lo + hi);
    if (gait::smoothStep5(mid) < level) lo = mid;
    else hi = mid;
  }
  stop_at_ = t_ - (1.0f - lo) * FADE_S;
}

void Player::reset() {
  type_ = Type::None;
  t_ = 0.0f;
  stop_at_ = -1.0f;
}

Offset Player::update(float dt) {
  Offset o;
  if (type_ == Type::None) return o;
  t_ += dt;
  const float d = duration();
  const float end = stop_at_ >= 0.0f ? stop_at_ + FADE_S : d;
  if (t_ >= end) {
    reset();
    return o;
  }
  // Hüllkurve: weich ein, weich aus (Abbruch: nur noch aus, ab dem damaligen Pegel)
  float env;
  if (stop_at_ >= 0.0f) {
    env = gait::smoothStep5((end - t_) / FADE_S);
  } else {
    env = gait::smoothStep5(t_ / FADE_S);
    if (t_ > d - FADE_S) env *= gait::smoothStep5((d - t_) / FADE_S);
  }

  const float t = t_;
  switch (type_) {
    case Type::Circle: {  // Körper kreist zweimal (Schwerpunkt wandert im Kreis)
      const float w = TWO_PI / 2.5f;
      o.x = 20.0f * std::sin(w * t) * env;
      o.y = 20.0f * std::cos(w * t) * env;
      break;
    }
    case Type::Nod:  // Ja: dreimal nicken
      o.pitch = -9.0f * std::sin(TWO_PI * 1.2f * t) * env;
      break;
    case Type::Shake:  // Nein: dreimal den Kopf schütteln
      o.yaw = 12.0f * std::sin(TWO_PI * 1.2f * t) * env;
      break;
    case Type::Pushup: {  // Liegestütz: dreimal tief und wieder hoch
      const float s = std::sin(TWO_PI * 0.55f * t * 0.5f);
      o.z = -22.0f * s * s * env;
      o.pitch = 3.0f * s * s * env;  // Nase etwas höher, sieht kräftiger aus
      break;
    }
    case Type::Dance: {  // Hüften schwingen, wippen, drehen
      const float w = TWO_PI * 0.8f;
      o.y = 12.0f * std::sin(w * t) * env;
      o.roll = 7.0f * std::sin(w * t) * env;
      const float b = std::sin(w * t);
      o.z = -6.0f * b * b * env;
      o.yaw = 8.0f * std::sin(w * 0.5f * t) * env;
      break;
    }
    default:
      break;
  }
  return o;
}

}  // namespace gestures
