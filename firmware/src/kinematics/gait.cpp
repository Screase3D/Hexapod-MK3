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
// Ändert sich das Tempo, während ein Fuß steht, wandert er weiter als ±v*half um
// seinen Neutralpunkt: Dauert das Beschleunigen mindestens eine Standphase, höchstens
// 25 % (ebenso beim Anlaufen mit der Anlauf-Rampe). Die Reichweitengrenze hält
// diese Reserve frei.
constexpr float ACCEL_MARGIN = 1.25f;
// Schrittfrequenz passt sich dem Tempo an: Langsam werden die Schritte nicht
// beliebig kurz und hektisch, sondern die Beine schwingen ruhiger (bis zur
// doppelten Schrittdauer) und halten mindestens die halbe Schrittlänge – weniger
// Schritte je Strecke, natürlicher und sparsamer.
constexpr float CADENCE_MAX = 2.0f;      // längste Schrittdauer = 2 * swing_s
constexpr float STRIDE_KEEP = 0.5f;      // Mindest-Schrittlänge (Anteil von max_stride)
constexpr float CADENCE_RATE = 1.0f;     // Änderung je Sekunde

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

namespace {
// Kleinster Kniewinkel (Grad) eines Schritts genau radial von stance-e nach
// stance+e mit den Schwungprofilen; < 0: Punkt unterwegs nicht erreichbar
float minKneeRadialStep(const kin::LegGeometry& g, float stance, float e, float height, float lift) {
  constexpr int N = 32;
  const float F = g.femur, T = g.tibia;
  float best = 1e9f;
  for (int i = 0; i <= N; ++i) {
    const float u = static_cast<float>(i) / N;
    const float rf = stance - e + 2.0f * e * smoothStep5(u) - g.coxa;  // ab Femur-Achse
    const float z = height - lift * liftProfile(u);
    const float d2 = rf * rf + z * z;
    const float c = (F * F + T * T - d2) / (2.0f * F * T);
    if (c >= 1.0f) return -1.0f;  // Knie ganz zusammengeklappt
    if (c <= -1.0f) continue;     // gestreckt: begrenzt maxFootRadius
    const float knee = std::acos(c) * 57.2957795f;
    if (knee < best) best = knee;
  }
  return best;
}
}  // namespace

float minFootRadius(const kin::LegGeometry& g, float stance, float height, float step_height,
                    float knee_min_deg) {
  if (minKneeRadialStep(g, stance, 0.0f, height, step_height) < knee_min_deg) return stance;
  float lo = 0.0f, hi = stance - g.coxa;  // Auslenkung nach innen
  if (minKneeRadialStep(g, stance, hi, height, step_height) >= knee_min_deg) return stance - hi;
  for (int it = 0; it < 18; ++it) {
    const float mid = 0.5f * (lo + hi);
    if (minKneeRadialStep(g, stance, mid, height, step_height) >= knee_min_deg) lo = mid;
    else hi = mid;
  }
  return stance - lo;
}

float maxFootRadius(const kin::LegGeometry& g, float height) {
  const float reach = 0.97f * (g.femur + g.tibia);
  return g.coxa + (reach > height ? std::sqrt(reach * reach - height * height) : 0.0f);
}

void Gait::reset(const kin::Vec3 neutral[NUM_LEGS]) {
  for (int i = 0; i < NUM_LEGS; ++i) {
    legs_[i] = Leg();
    legs_[i].pos = {neutral[i].x, neutral[i].y, 0.0f};
  }
  for (int k = 0; k < 3; ++k) cmd_[k] = cmd_ramped_[k] = 0.0f;
  vx_ = vy_ = wz_ = 0.0f;
  reach_k_ = 1.0f;
  cadence_ = 1.0f;
  phase_ = 0.0f;
  idle_ = true;
  starting_ = true;
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

void Gait::setReach(const kin::Vec3 coxa[NUM_LEGS], float min_radius, float max_radius) {
  for (int i = 0; i < NUM_LEGS; ++i) coxa_[i] = coxa[i];
  min_radius_ = min_radius;
  max_radius_ = max_radius;
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
  // Befehl rampen -> weiches Anfahren und Anhalten.
  // Beschleunigen dauert mindestens eine Standphase (siehe ACCEL_MARGIN).
  // Beim Anlaufen stehen alle Füße in der Neutralstellung; das Bein mit dem
  // spätesten Schwungfenster wandert bis zu seinem ersten Schritt mit dem Körper
  // mit. Damit es dabei nicht weiter kommt als im eingeschwungenen Lauf (halbe
  // Standphase), wird im ersten Zyklus langsamer beschleunigt:
  // Rampe >= 2 * spätester Schwungbeginn - Standphase (+20 % Reserve).
  const int t = static_cast<int>(active_type_);
  const float stance_nom = dutyFactor(active_type_) * cycleTime(active_type_, params_.swing_s);
  const float stance_s = stance_nom * cadence_;  // mit der aktuellen Schrittfrequenz
  float ramp_up = std::fmax(params_.ramp_s, stance_s);
  if (starting_) {
    float last = 0.0f;
    for (int i = 0; i < NUM_LEGS; ++i) last = std::fmax(last, SWING_START[t][i]);
    ramp_up = std::fmax(ramp_up, 1.2f * (2.0f * last - (1.0f - SWING_FRACTION[t])) *
                                     cycleTime(active_type_, params_.swing_s * cadence_));
  }
  for (int k = 0; k < 3; ++k) {
    bool accel = cmd_[k] * cmd_ramped_[k] >= 0.0f && std::fabs(cmd_[k]) > std::fabs(cmd_ramped_[k]);
    cmd_ramped_[k] = approach(cmd_ramped_[k], cmd_[k], dt / (accel ? ramp_up : params_.ramp_s));
  }

  // Höchstgeschwindigkeit: in einer Standphase darf ein Fuß höchstens
  // max_stride zurücklegen (Befehl 1 = Tempo bei der eingestellten Schrittdauer)
  const float foot_max = params_.max_stride / stance_nom;
  float r_max = 1.0f;
  for (int i = 0; i < NUM_LEGS; ++i) {
    float r = std::hypot(neutral[i].x, neutral[i].y);
    if (r > r_max) r_max = r;
  }
  const float turn_max = foot_max / r_max;

  // Befehl -> Geschwindigkeit; Fahren + Drehen gleichzeitig: schnellsten Fuß auf
  // foot_max begrenzen
  auto fastestFoot = [&](const float v[3]) {
    float fastest = 0.0f;
    for (int i = 0; i < NUM_LEGS; ++i) {
      float f = std::hypot(v[0] - v[2] * neutral[i].y, v[1] + v[2] * neutral[i].x);
      if (f > fastest) fastest = f;
    }
    return fastest;
  };
  auto toVelocity = [&](const float c[3], float v[3]) {
    v[0] = c[0] * foot_max;
    v[1] = c[1] * foot_max;
    v[2] = c[2] * turn_max;
    float fastest = fastestFoot(v);
    if (fastest > foot_max) {
      for (int a = 0; a < 3; ++a) v[a] *= foot_max / fastest;
    }
  };
  float v_cur[3], v_goal[3];
  toVelocity(cmd_ramped_, v_cur);
  toVelocity(cmd_, v_goal);

  // Schrittfrequenz: ruhiger, je langsamer (beim Anhalten unverändert lassen)
  const bool commanded = std::fabs(cmd_[0]) > MOVING_EPS || std::fabs(cmd_[1]) > MOVING_EPS ||
                         std::fabs(cmd_[2]) > MOVING_EPS;
  if (commanded) {
    const float ratio = fastestFoot(v_cur) / foot_max;
    float target = ratio > 1e-3f ? STRIDE_KEEP / ratio : CADENCE_MAX;
    target = target < 1.0f ? 1.0f : (target > CADENCE_MAX ? CADENCE_MAX : target);
    cadence_ = approach(cadence_, target, CADENCE_RATE * dt);
  }
  // Bei der aktuellen Schrittfrequenz höchstens max_stride je Standphase
  // (folgt die Frequenz einer Tempoänderung noch, wird kurz gebremst)
  const float foot_max_now = params_.max_stride / stance_s;
  const float fastest_now = fastestFoot(v_cur);
  if (fastest_now > foot_max_now) {
    for (int a = 0; a < 3; ++a) v_cur[a] *= foot_max_now / fastest_now;
  }

  // Reichweite: kein Fuß kommt der Coxa-Achse zu nahe. Ein Fuß pendelt um ±v*half
  // um seinen Neutralpunkt; er steht bis zu einen Regelschritt länger als die
  // Standphase, beim Beschleunigen etwas weiter (ACCEL_MARGIN). Die Grenze gilt
  // schon für das Fahrziel, damit die Rampe gleichmäßig bis zum erlaubten Tempo
  // beschleunigt (sonst wäre das Beschleunigen früher zu Ende als geplant).
  const float half = (0.5f * stance_s + dt) * ACCEL_MARGIN;
  const float target = std::fmin(reachScale(v_goal[0], v_goal[1], v_goal[2], half, neutral),
                                 reachScale(v_cur[0], v_cur[1], v_cur[2], half, neutral));
  // sinken sofort, steigen nur mit der Rampe (kein Tempo-Sprung)
  reach_k_ = target < reach_k_ ? target : std::fmin(target, reach_k_ + dt / ramp_up);
  vx_ = v_cur[0] * reach_k_;
  vy_ = v_cur[1] * reach_k_;
  wz_ = v_cur[2] * reach_k_;
  // Anzeige: Höchsttempo geradeaus, seitwärts bzw. Drehrate auf der Stelle
  // (bei voller Geschwindigkeit läuft die eingestellte Schrittfrequenz)
  const float half_nom = (0.5f * stance_nom + dt) * ACCEL_MARGIN;
  max_speed_ = foot_max * reachScale(foot_max, 0.0f, 0.0f, half_nom, neutral);
  max_side_ = foot_max * reachScale(0.0f, foot_max, 0.0f, half_nom, neutral);
  max_turn_ = turn_max * reachScale(0.0f, 0.0f, turn_max, half_nom, neutral);
  nominal_speed_ = foot_max;
}

float Gait::reachScale(float vx, float vy, float wz, float half_stance_s,
                       const kin::Vec3 neutral[NUM_LEGS]) const {
  if (min_radius_ < 0.0f) return 1.0f;
  float s = 1.0f;
  for (int i = 0; i < NUM_LEGS; ++i) {
    // Fuß pendelt in der Standphase um den Neutralpunkt n: n ± t*d
    const float dx = (vx - wz * neutral[i].y) * half_stance_s;
    const float dy = (vy + wz * neutral[i].x) * half_stance_s;
    const float rx = neutral[i].x - coxa_[i].x;
    const float ry = neutral[i].y - coxa_[i].y;
    const float r2 = rx * rx + ry * ry;
    const float m2 = min_radius_ * min_radius_;
    const float M2 = max_radius_ * max_radius_;
    if (r2 <= m2 || r2 >= M2) return 0.0f;  // steht schon am Rand der Reichweite
    const float dd = dx * dx + dy * dy;
    if (dd < 1e-9f) continue;
    const float rd = std::fabs(rx * dx + ry * dy);
    // innen: |r - t*d|² >= m²  <=>  dd*t² - 2*rd*t + (r² - m²) >= 0
    const float disc = rd * rd - dd * (r2 - m2);
    if (disc > 0.0f) {
      const float t = (rd - std::sqrt(disc)) / dd;
      if (t < s) s = t;
    }
    // außen: |r + t*d|² <= M²  <=>  dd*t² + 2*rd*t - (M² - r²) <= 0
    const float t_out = (-rd + std::sqrt(rd * rd + dd * (M2 - r2))) / dd;
    if (t_out < s) s = t_out;
  }
  return s < 0.0f ? 0.0f : s;
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

  const float cycle = cycleTime(active_type_, params_.swing_s * cadence_);
  const float next_phase = phase_ + dt / cycle;
  if (next_phase >= 1.0f) starting_ = false;  // jedes Bein hatte sein Schwungfenster
  phase_ = frac(next_phase);
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
    starting_ = true;
    phase_ = 0.0f;
    vx_ = vy_ = wz_ = 0.0f;
    for (int i = 0; i < NUM_LEGS; ++i) legs_[i].pos = {neutral[i].x, neutral[i].y, 0.0f};
    active_type_ = params_.type;
  }
}

}  // namespace gait
