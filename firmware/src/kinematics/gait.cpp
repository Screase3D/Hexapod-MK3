#include "gait.h"

#include <cmath>
#include <cstring>

namespace gait {
namespace {

constexpr float SETTLE_TOL = 2.0f;     // mm: Fuß gilt als "in Neutralstellung"
// Weicht ein Fuß im Stillstand weiter ab (Neutralpunkt sprunghaft geändert,
// z. B. beim Aufstehen), wird er mit Schritten umgesetzt statt verschoben
constexpr float REPOSITION_MIN = 10.0f;
constexpr float MOVING_EPS = 1e-3f;    // normierter Befehl, ab dem gelaufen wird
constexpr float LATE_START = 0.25f;    // Schwungfenster erst so spät erreicht -> aussetzen
constexpr float MAX_DT = 0.05f;        // größere Zeitschritte (Hänger) begrenzen

// Beginn der Schwungphase je Bein als Anteil des Zyklus.
// Beine: 0 vorne links, 1 Mitte links, 2 hinten links, 3 vorne rechts, 4 Mitte rechts, 5 hinten rechts
constexpr float SWING_START[NUM_TYPES][NUM_LEGS] = {
    // Dreifuß: {vl, hl, mr} und {ml, vr, hr} abwechselnd
    {0.0f, 0.5f, 0.0f, 0.5f, 0.0f, 0.5f},
    // Ripple: je Seite hinten -> Mitte -> vorne, Seiten um einen halben Zyklus versetzt
    {2.0f / 3.0f, 1.0f / 3.0f, 0.0f, 1.0f / 6.0f, 5.0f / 6.0f, 0.5f},
    // Welle: hl, ml, vl, hr, mr, vr nacheinander
    {2.0f / 6.0f, 1.0f / 6.0f, 0.0f, 5.0f / 6.0f, 4.0f / 6.0f, 3.0f / 6.0f},
};

// Anteil der Schwungphase am Zyklus
constexpr float SWING_FRACTION[NUM_TYPES] = {0.5f, 1.0f / 3.0f, 1.0f / 6.0f};
constexpr float PHASE_EPS = 1e-5f;  // Rundung an den Fenstergrenzen

float frac(float v) { return v - std::floor(v); }
float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float approach(float cur, float target, float step) {
  float d = target - cur;
  if (d > step) return cur + step;
  if (d < -step) return cur - step;
  return target;
}

float distXY(const kin::Vec3& a, const kin::Vec3& b) {
  return std::hypot(a.x - b.x, a.y - b.y);
}

}  // namespace

const char* typeName(Type t) {
  switch (t) {
    case Type::Ripple: return "ripple";
    case Type::Wave:   return "wave";
    default:           return "tripod";
  }
}

bool typeFromName(const char* name, Type& out) {
  if (!name) return false;
  for (int i = 0; i < NUM_TYPES; ++i) {
    if (std::strcmp(name, typeName(static_cast<Type>(i))) == 0) {
      out = static_cast<Type>(i);
      return true;
    }
  }
  return false;
}

float dutyFactor(Type t) { return 1.0f - SWING_FRACTION[static_cast<int>(t)]; }

float cycleTime(Type t, float swing_s) { return swing_s / SWING_FRACTION[static_cast<int>(t)]; }

float smoothStep5(float u) {
  u = clamp01(u);
  return u * u * u * (10.0f + u * (-15.0f + 6.0f * u));
}

float liftProfile(float u) {
  u = clamp01(u);
  float a = u * (1.0f - u);
  return 16.0f * a * a;
}

void Gait::reset(const kin::Vec3 neutral[NUM_LEGS]) {
  for (int i = 0; i < NUM_LEGS; ++i) {
    legs_[i] = Leg();
    legs_[i].pos = {neutral[i].x, neutral[i].y, 0.0f};
  }
  for (int k = 0; k < 3; ++k) cmd_[k] = cmd_ramped_[k] = 0.0f;
  vx_ = vy_ = wz_ = 0.0f;
  phase_ = 0.0f;
  idle_ = true;
  active_type_ = params_.type;
}

void Gait::setParams(const Params& p) {
  params_ = p;
  if (params_.swing_s < 0.15f) params_.swing_s = 0.15f;
  if (params_.step_height < 0.0f) params_.step_height = 0.0f;
  if (params_.max_stride < 1.0f) params_.max_stride = 1.0f;
  if (params_.ramp_s < 0.05f) params_.ramp_s = 0.05f;
  if (idle_) active_type_ = params_.type;
}

void Gait::setCommand(float vx, float vy, float turn) {
  auto lim = [](float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); };
  cmd_[0] = lim(vx);
  cmd_[1] = lim(vy);
  cmd_[2] = lim(turn);
}

uint8_t Gait::swingMask() const {
  uint8_t m = 0;
  for (int i = 0; i < NUM_LEGS; ++i) {
    if (legs_[i].swing) m |= 1 << i;
  }
  return m;
}

void Gait::computeVelocity(float dt, const kin::Vec3 neutral[NUM_LEGS]) {
  // Befehl rampen -> weiches Anfahren und Anhalten
  float step = dt / params_.ramp_s;
  for (int k = 0; k < 3; ++k) cmd_ramped_[k] = approach(cmd_ramped_[k], cmd_[k], step);

  // Höchstgeschwindigkeit: in einer Standphase darf ein Fuß höchstens
  // max_stride zurücklegen
  float stance_s = dutyFactor(active_type_) * cycleTime(active_type_, params_.swing_s);
  float foot_max = params_.max_stride / stance_s;
  float r_max = 1.0f;
  for (int i = 0; i < NUM_LEGS; ++i) {
    float r = std::hypot(neutral[i].x, neutral[i].y);
    if (r > r_max) r_max = r;
  }
  max_speed_ = foot_max;
  max_turn_ = foot_max / r_max;

  float vx = cmd_ramped_[0] * max_speed_;
  float vy = cmd_ramped_[1] * max_speed_;
  float wz = cmd_ramped_[2] * max_turn_;

  // Fahren + Drehen gleichzeitig: schnellsten Fuß auf foot_max begrenzen
  float fastest = 0.0f;
  for (int i = 0; i < NUM_LEGS; ++i) {
    float fx = vx - wz * neutral[i].y;
    float fy = vy + wz * neutral[i].x;
    float f = std::hypot(fx, fy);
    if (f > fastest) fastest = f;
  }
  float k = fastest > foot_max ? foot_max / fastest : 1.0f;
  vx_ = vx * k;
  vy_ = vy * k;
  wz_ = wz * k;
}

void Gait::update(float dt, const kin::Vec3 neutral[NUM_LEGS]) {
  if (dt <= 0.0f) return;
  if (dt > MAX_DT) dt = MAX_DT;
  computeVelocity(dt, neutral);
  bool moving = std::fabs(cmd_ramped_[0]) > MOVING_EPS || std::fabs(cmd_ramped_[1]) > MOVING_EPS ||
                std::fabs(cmd_ramped_[2]) > MOVING_EPS;

  if (idle_) {
    float dev = 0.0f;
    for (int i = 0; i < NUM_LEGS; ++i) {
      float d = distXY(legs_[i].pos, neutral[i]);
      if (d > dev) dev = d;
    }
    if (!moving && dev <= REPOSITION_MIN) {
      // Stillstand: Füße folgen kleinen Änderungen der Neutralpunkte direkt
      // (z. B. Regler Fußabstand, max. 80 mm/s)
      for (int i = 0; i < NUM_LEGS; ++i) legs_[i].pos = {neutral[i].x, neutral[i].y, 0.0f};
      active_type_ = params_.type;
      return;
    }
    idle_ = false;
    phase_ = 0.0f;  // erste Beingruppe hebt sofort ab
    for (auto& l : legs_) l.in_window = false;
  }

  // Standbeine bleiben auf dem Boden stehen: im Lauf-System bewegen sie sich
  // entgegen der Körperbewegung (verschieben, dann um -wz*dt drehen)
  float c = std::cos(-wz_ * dt), s = std::sin(-wz_ * dt);
  for (auto& l : legs_) {
    if (l.swing) continue;
    float x = l.pos.x - vx_ * dt;
    float y = l.pos.y - vy_ * dt;
    l.pos = {c * x - s * y, s * x + c * y, 0.0f};
  }

  const float cycle = cycleTime(active_type_, params_.swing_s);
  phase_ = frac(phase_ + dt / cycle);
  const int t = static_cast<int>(active_type_);
  const float sigma = SWING_FRACTION[t];
  const float half_stance = 0.5f * (1.0f - sigma) * cycle;

  for (int i = 0; i < NUM_LEGS; ++i) {
    Leg& l = legs_[i];
    // Aufsetzpunkt: so weit vor der Neutralstellung, dass die folgende
    // Standphase symmetrisch um sie herum verläuft
    kin::Vec3 target = {neutral[i].x + (vx_ - wz_ * neutral[i].y) * half_stance,
                        neutral[i].y + (vy_ + wz_ * neutral[i].x) * half_stance, 0.0f};
    float p = frac(phase_ - SWING_START[t][i]);
    // Fenster minimal kürzen: an der Grenze nie zwei Gruppen gleichzeitig in der Luft
    bool win = p < sigma - PHASE_EPS;
    float u = p / sigma;

    if (win && !l.in_window) {
      // Schwungfenster beginnt: nur abheben, wenn nötig und nicht mitten drin
      bool needed = moving || distXY(l.pos, neutral[i]) > SETTLE_TOL;
      if (needed && u < LATE_START) {
        l.swing = true;
        l.lift = l.pos;
      }
    } else if (!win && l.in_window && l.swing) {
      l.swing = false;  // aufsetzen
      l.pos = target;
    }
    l.in_window = win;

    if (l.swing) {
      // Heben und Vorschwingen gleichzeitig, beide mit weichem Anfang und Ende
      // (früh nach außen zu schwingen entlastet den Femur beim Abheben nah am Körper)
      float h = smoothStep5(u);
      l.pos.x = l.lift.x + (target.x - l.lift.x) * h;
      l.pos.y = l.lift.y + (target.y - l.lift.y) * h;
      l.pos.z = params_.step_height * liftProfile(u);
    }
  }

  if (!moving) {
    // Angekommen? Dann anhalten (kein Bein in der Luft, alle in Neutralstellung)
    for (int i = 0; i < NUM_LEGS; ++i) {
      if (legs_[i].swing || distXY(legs_[i].pos, neutral[i]) > SETTLE_TOL) return;
    }
    idle_ = true;
    phase_ = 0.0f;
    vx_ = vy_ = wz_ = 0.0f;
    for (int i = 0; i < NUM_LEGS; ++i) legs_[i].pos = {neutral[i].x, neutral[i].y, 0.0f};
    active_type_ = params_.type;
  }
}

}  // namespace gait
