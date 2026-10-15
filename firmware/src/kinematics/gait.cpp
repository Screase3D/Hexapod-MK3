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
constexpr float MAX_DT = 0.05f;        // größere Zeitschritte (Hänger) begrenzen
// Gangwechsel im Lauf: Die Phase eines Standbeins läuft höchstens 10 % langsamer
// (Standphase bis zu 1/0,9 = 1,11-mal so lang, der Fuß wandert kaum weiter nach hinten)
// bzw. 60 % schneller (kürzer: unkritisch) – Beine, die hinterher sind, holen auf;
// solange die Beine noch nicht im neuen Muster sind, läuft der Roboter mit 75 % des
// Tempos (Reserve, falls ein Bein auf seinen Nachbarn warten muss).
constexpr float PHASE_GAIN = 8.0f;       // Korrektur je Zyklus und Abweichung
constexpr float PHASE_SLOW = 0.1f, PHASE_FAST = 0.6f;
constexpr float PHASE_SNAP = 2e-3f;      // näher am Muster: Versatz exakt übernehmen
constexpr float TRANS_SPEED = 0.75f;
constexpr float VMAX_RAMP_S = 0.8f;      // Tempo-Anpassung beim Gangwechsel: volle Spanne in 0,8 s
constexpr float TWO_PI = 6.28318531f;
constexpr float REACH_GLIDE = 40.0f;     // mm/s: so schnell wird der erlaubte Bereich im Lauf enger
constexpr float URGENT_MIN_STEP = 5.0f;  // mm: kürzere Zwischenschritte bringen nichts
// Nachbarbeine (Reihenfolge um den Körper: vl, ml, hl, hr, mr, vr)
constexpr int NEIGHBOR_A[NUM_LEGS] = {1, 0, 1, 0, 3, 2};
constexpr int NEIGHBOR_B[NUM_LEGS] = {3, 2, 5, 4, 5, 4};
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
    // Klettern: wie Ripple (immer vier Füße am Boden)
    {2.0f / 3.0f, 1.0f / 3.0f, 0.0f, 1.0f / 6.0f, 5.0f / 6.0f, 0.5f},
};

// Anteil der Schwungphase am Zyklus
constexpr float SWING_FRACTION[NUM_TYPES] = {0.5f, 1.0f / 3.0f, 1.0f / 6.0f, 1.0f / 3.0f};
// Schrittdauer je Gangart (mal swing_s): Klettern schwingt langsamer (weiter Weg nach oben)
constexpr float SWING_SCALE[NUM_TYPES] = {1.0f, 1.0f, 1.0f, CLIMB_SWING_SCALE};
// Kastenbahn: Heben bis CLIMB_UP, vorschwingen zwischen CLIMB_GO und 1 - CLIMB_GO (Anteil Schwung)
constexpr float CLIMB_UP = 0.35f;
constexpr float CLIMB_GO = 0.25f;
constexpr float CLIMB_FOLLOW = 150.0f;  // mm/s: so schnell folgt der vorgeschwungene Fuß dem Aufsetzpunkt
constexpr float PHASE_EPS = 1e-5f;  // Rundung an den Fenstergrenzen

float frac(float v) { return v - std::floor(v); }
float wrapHalf(float v) { return v - std::floor(v + 0.5f); }  // -0.5..0.5
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
    case Type::Climb:  return "climb";
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

float cycleTime(Type t, float swing_s) {
  return swing_s * SWING_SCALE[static_cast<int>(t)] / SWING_FRACTION[static_cast<int>(t)];
}

float smoothStep5(float u) {
  u = clamp01(u);
  return u * u * u * (10.0f + u * (-15.0f + 6.0f * u));
}

float liftProfile(float u) {
  u = clamp01(u);
  float a = u * (1.0f - u);
  return 16.0f * a * a;
}

float climbLift(float u) {
  u = clamp01(u);
  return smoothStep5((u < 0.5f ? u : 1.0f - u) / CLIMB_UP);
}

float climbTravel(float u) { return smoothStep5((u - CLIMB_GO) / (1.0f - 2.0f * CLIMB_GO)); }

float climbOut(float u, float r_path, float r_neutral) {
  // mit dem Heben (das Knie knickt schon auf halber Höhe am weitesten ein)
  return climbLift(u) * std::fmax(CLIMB_OUT, r_neutral + CLIMB_OUT_MIN - r_path);
}

namespace {
// Schritt genau radial von stance-e nach stance+e mit den Schwungprofilen: hält er
// Knie (>= knee_min) und Femur (<= femur_max) ein? (Grad)
bool radialStepOk(const kin::LegGeometry& g, float stance, float e, float height, float lift, bool climb,
                  float knee_min, float femur_max) {
  constexpr int N = 32;
  const float F = g.femur, T = g.tibia;
  for (int i = 0; i <= N; ++i) {
    const float u = static_cast<float>(i) / N;
    float rf, z;  // ab Femur-Achse
    if (climb) {
      const float rp = stance - e + 2.0f * e * climbTravel(u);
      rf = rp + climbOut(u, rp, stance) - g.coxa;
      z = height - lift * climbLift(u);
    } else {
      rf = stance - e + 2.0f * e * smoothStep5(u) - g.coxa;
      z = height - lift * liftProfile(u);
    }
    const float d2 = rf * rf + z * z;
    const float c = (F * F + T * T - d2) / (2.0f * F * T);
    if (c >= 1.0f) return false;  // Knie ganz zusammengeklappt
    if (c <= -1.0f) continue;     // gestreckt: begrenzt maxFootRadius
    if (std::acos(c) * 57.2957795f < knee_min) return false;
    if (femur_max < 180.0f) {
      // Femur über der Waagerechten: Richtung zum Fuß + Winkel zwischen Femur und Fuß-Linie
      const float d = std::sqrt(d2);
      const float a = (F * F + d2 - T * T) / (2.0f * F * d);
      const float femur = (std::atan2(-z, rf) + std::acos(a < -1.0f ? -1.0f : (a > 1.0f ? 1.0f : a))) * 57.2957795f;
      if (femur > femur_max) return false;
    }
  }
  return true;
}
}  // namespace

float minFootRadius(const kin::LegGeometry& g, float stance, float height, float step_height,
                    float knee_min_deg, bool climb, float femur_max_deg) {
  auto ok = [&](float e) {
    return radialStepOk(g, stance, e, height, step_height, climb, knee_min_deg, femur_max_deg);
  };
  if (!ok(0.0f)) return stance;
  float lo = 0.0f, hi = stance - g.coxa;  // Auslenkung nach innen
  if (ok(hi)) return stance - hi;
  for (int it = 0; it < 18; ++it) {
    const float mid = 0.5f * (lo + hi);
    if (ok(mid)) lo = mid;
    else hi = mid;
  }
  return stance - lo;
}

float climbHeight(const kin::LegGeometry& g, float stance, float height, float inward, float knee_min_deg,
                  float femur_max_deg, float lo_mm, float hi_mm) {
  auto ok = [&](float lift) {
    return radialStepOk(g, stance, inward, height, lift, true, knee_min_deg, femur_max_deg);
  };
  if (hi_mm < lo_mm || !ok(lo_mm)) return lo_mm;
  if (ok(hi_mm)) return hi_mm;
  float lo = lo_mm, hi = hi_mm;
  for (int it = 0; it < 12; ++it) {
    const float mid = 0.5f * (lo + hi);
    if (ok(mid)) lo = mid;
    else hi = mid;
  }
  return lo;
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
  brake_ = 1.0f;
  goal_k_ = 1.0f;
  limit_now_ = 0.0f;
  cadence_ = 1.0f;
  phase_ = 0.0f;
  vmax_ = 0.0f;
  converging_ = switching_ = false;
  phase_err_ = 0.0f;
  idle_ = true;
  starting_ = true;
  active_type_ = params_.type;
}

float Gait::footMax(Type t) const {
  return stride(t) / (dutyFactor(t) * cycleTime(t, params_.swing_s));
}

bool Gait::liftAllowed(int leg) const {
  return !legs_[NEIGHBOR_A[leg]].swing && !legs_[NEIGHBOR_B[leg]].swing;
}

void Gait::setParams(const Params& p) {
  params_ = p;
  if (params_.swing_s < 0.15f) params_.swing_s = 0.15f;
  if (params_.step_height < 0.0f) params_.step_height = 0.0f;
  if (params_.max_stride < 1.0f) params_.max_stride = 1.0f;
  if (params_.ramp_s < 0.05f) params_.ramp_s = 0.05f;
  if (params_.climb_height < params_.step_height) params_.climb_height = params_.step_height;
  if (idle_) active_type_ = params_.type;
}

void Gait::setReach(const kin::Vec3 coxa[NUM_LEGS], float min_radius, float max_radius) {
  for (int i = 0; i < NUM_LEGS; ++i) coxa_[i] = coxa[i];
  // Wird der Bereich enger (höherer Fußhub, andere Körperhöhe), gleitet die Grenze im Lauf
  // dorthin (update): sonst stünden Füße schlagartig "außerhalb" -> Vollbremsung
  min_target_ = min_radius;
  max_target_ = max_radius;
  if (idle_ || min_radius_ < 0.0f || min_radius < min_radius_) min_radius_ = min_radius;
  if (idle_ || max_radius_ <= 0.0f || max_radius > max_radius_) max_radius_ = max_radius;
}

void Gait::setSwingGuard(const kin::LegGeometry& g, float height, float knee_min_deg) {
  // kleinster Abstand Femur-Achse <-> Fuß, bei dem das Knie knee_min_deg erreicht
  guard_coxa_ = g.coxa;
  guard_h_ = height;
  guard_d2_ = g.femur * g.femur + g.tibia * g.tibia -
              2.0f * g.femur * g.tibia * std::cos(knee_min_deg * 0.0174532925f);
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
  // Wechsel von/zu Klettern: erst anhalten (Beine in die Neutralstellung), dann in der neuen
  // Gangart wieder anlaufen – die Schritte sind zu verschieden für ein fließendes Umtakten
  static const float ZERO[3] = {0, 0, 0};
  const float* cmd = stopSwitch() ? ZERO : cmd_;
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
    bool accel = cmd[k] * cmd_ramped_[k] >= 0.0f && std::fabs(cmd[k]) > std::fabs(cmd_ramped_[k]);
    cmd_ramped_[k] = approach(cmd_ramped_[k], cmd[k], dt / (accel ? ramp_up : params_.ramp_s));
  }

  float r_max = 1.0f;
  for (int i = 0; i < NUM_LEGS; ++i) {
    float r = std::hypot(neutral[i].x, neutral[i].y);
    if (r > r_max) r_max = r;
  }
  // Befehl -> Geschwindigkeit bei Fußgeschwindigkeit fm; Fahren + Drehen gleichzeitig:
  // schnellsten Fuß auf fm begrenzen
  auto fastestFoot = [&](const float v[3]) {
    float fastest = 0.0f;
    for (int i = 0; i < NUM_LEGS; ++i) {
      float f = std::hypot(v[0] - v[2] * neutral[i].y, v[1] + v[2] * neutral[i].x);
      if (f > fastest) fastest = f;
    }
    return fastest;
  };
  auto toVelocity = [&](const float c[3], float fm, float v[3]) {
    v[0] = c[0] * fm;
    v[1] = c[1] * fm;
    v[2] = c[2] * fm / r_max;
    float fastest = fastestFoot(v);
    if (fastest > fm) {
      for (int a = 0; a < 3; ++a) v[a] *= fm / fastest;
    }
  };

  // Höchstgeschwindigkeit: in einer Standphase darf ein Fuß höchstens
  // max_stride zurücklegen (Befehl 1 = Tempo bei der eingestellten Schrittdauer).
  // Gangwechsel: bis die Beine im neuen Muster sind, 75 % des Tempos der langsameren
  // der beiden Gangarten (Reserve für längere Standphasen); vor dem Umtakten in eine
  // langsamere Gangart muss das Tempo auch dort in die Reichweite passen. Die Grenze
  // gleitet dorthin statt zu springen.
  const float fm_active = stride(active_type_) / stance_nom;
  const bool pending = !idle_ && params_.type != active_type_ && !stopSwitch();  // (Klettern: hält nur an)
  float fm_target = fm_active;
  if (pending) {
    fm_target = std::fmin(fm_active, footMax(params_.type));
    float vg[3];
    toVelocity(cmd, fm_target, vg);
    const float st_new = dutyFactor(params_.type) * cycleTime(params_.type, params_.swing_s);
    fm_target *= TRANS_SPEED * reachScale(vg[0], vg[1], vg[2], (0.5f * st_new + dt) * ACCEL_MARGIN, neutral);
  } else if (!idle_ && switching_) {
    fm_target = TRANS_SPEED * std::fmin(fm_active, footMax(from_type_));
  }
  if (idle_ || vmax_ <= 0.0f) vmax_ = fm_target;
  else vmax_ = approach(vmax_, fm_target, std::fmax(fm_active, footMax(params_.type)) / VMAX_RAMP_S * dt);
  const float foot_max = vmax_;

  float v_cur[3], v_goal[3];
  toVelocity(cmd_ramped_, foot_max, v_cur);
  toVelocity(cmd, foot_max, v_goal);

  // Schrittfrequenz: ruhiger, je langsamer (beim Anhalten unverändert lassen)
  const bool commanded = std::fabs(cmd[0]) > MOVING_EPS || std::fabs(cmd[1]) > MOVING_EPS ||
                         std::fabs(cmd[2]) > MOVING_EPS;
  if (commanded) {
    // (vor dem Umtakten bezogen auf die bisherige Gangart -> ruhige Schritte beim Abbremsen)
    const float ratio = fastestFoot(v_cur) / (switching_ ? foot_max : fm_active);
    float target = ratio > 1e-3f ? STRIDE_KEEP / ratio : CADENCE_MAX;
    target = target < 1.0f ? 1.0f : (target > CADENCE_MAX ? CADENCE_MAX : target);
    cadence_ = approach(cadence_, target, CADENCE_RATE * dt);
  }
  // Bei der aktuellen Schrittfrequenz höchstens max_stride je Standphase
  // (folgt die Frequenz einer Tempoänderung noch, wird kurz gebremst). Sinkt die Grenze
  // plötzlich (längere Schrittdauer, höherer Fußhub), gleitet sie in ramp_s herunter statt
  // das Tempo in einem Regelschritt zu kappen – die Reserve ACCEL_MARGIN deckt das.
  const float foot_max_now = stride(active_type_) / stance_s;
  if (idle_ || limit_now_ <= 0.0f || foot_max_now >= limit_now_) limit_now_ = foot_max_now;
  else limit_now_ = std::fmax(foot_max_now, limit_now_ - fm_active * dt / params_.ramp_s);
  const float fastest_now = fastestFoot(v_cur);
  if (fastest_now > limit_now_) {
    for (int a = 0; a < 3; ++a) v_cur[a] *= limit_now_ / fastest_now;
  }

  // Reichweite: kein Fuß kommt der Coxa-Achse zu nahe. Ein Fuß pendelt um ±v*half
  // um seinen Neutralpunkt; er steht bis zu einen Regelschritt länger als die
  // Standphase, beim Beschleunigen etwas weiter (ACCEL_MARGIN). Die Grenze gilt
  // schon für das Fahrziel, damit die Rampe gleichmäßig bis zum erlaubten Tempo
  // beschleunigt (sonst wäre das Beschleunigen früher zu Ende als geplant).
  // Für das aktuelle Tempo gilt die Grenze sofort (Sicherheit); die des Fahrziels sinkt
  // höchstens mit der Rampe (Richtungswechsel z. B. vorwärts -> seitwärts: sanft abbremsen).
  const float half = (0.5f * stance_s + dt) * ACCEL_MARGIN;
  const float k_goal = reachScale(v_goal[0], v_goal[1], v_goal[2], half, neutral);
  if (idle_ || k_goal >= goal_k_) goal_k_ = k_goal;
  else goal_k_ = std::fmax(k_goal, goal_k_ - dt / params_.ramp_s);
  const float target = std::fmin(goal_k_, reachScale(v_cur[0], v_cur[1], v_cur[2], half, neutral));
  // sinken und steigen mit der Rampe (volle Spanne in ramp_s bzw. ramp_up) – kein
  // Tempo-Sprung, auch wenn sich Schrittdauer oder Fußhub im Lauf ändern. Kommt ein Fuß
  // dabei an den Rand, setzt er vorzeitig um bzw. bremst den Körper (brake_).
  reach_k_ = target < reach_k_ ? std::fmax(target, reach_k_ - dt / params_.ramp_s)
                               : std::fmin(target, reach_k_ + dt / ramp_up);
  reach_k_ *= brake_;  // ein Standbein muss auf seinen Nachbarn warten und ist schon nah am Rand
  brake_ = 1.0f;
  vx_ = v_cur[0] * reach_k_;
  vy_ = v_cur[1] * reach_k_;
  wz_ = v_cur[2] * reach_k_;
  // Anzeige: Höchsttempo geradeaus, seitwärts bzw. Drehrate auf der Stelle
  // (bei voller Geschwindigkeit läuft die eingestellte Schrittfrequenz)
  const float half_nom = (0.5f * stance_nom + dt) * ACCEL_MARGIN;
  const float turn_nom = fm_active / r_max;
  max_speed_ = fm_active * reachScale(fm_active, 0.0f, 0.0f, half_nom, neutral);
  max_side_ = fm_active * reachScale(0.0f, fm_active, 0.0f, half_nom, neutral);
  max_turn_ = turn_nom * reachScale(0.0f, 0.0f, turn_nom, half_nom, neutral);
  nominal_speed_ = fm_active;
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

float Gait::switchCadence(float dt, const kin::Vec3 neutral[NUM_LEGS]) const {
  float fastest = 0.0f;
  for (int i = 0; i < NUM_LEGS; ++i) {
    fastest = std::fmax(fastest, std::hypot(vx_ - wz_ * neutral[i].y, vy_ + wz_ * neutral[i].x));
  }
  const float fm = footMax(params_.type);
  if (fastest > fm * 1.001f + 1e-3f) return 0.0f;  // erst abbremsen
  // Schrittfrequenz wie im eingeschwungenen Lauf der neuen Gangart bei diesem Tempo,
  // aber keine langsameren Schwünge als bisher (kurze Zyklen: schnell im neuen Muster)
  float cad = fastest > 1e-3f ? std::fmin(cadence_, STRIDE_KEEP * fm / fastest) : cadence_;
  cad = cad < 1.0f ? 1.0f : (cad > CADENCE_MAX ? CADENCE_MAX : cad);
  if (fastest <= 1e-3f) return cad;
  auto reachOk = [&](float c) {
    const float stance = dutyFactor(params_.type) * cycleTime(params_.type, params_.swing_s * c);
    return reachScale(vx_, vy_, wz_, (0.5f * stance + dt) * ACCEL_MARGIN, neutral) >= 0.999f;
  };
  if (reachOk(cad)) return cad;
  return reachOk(1.0f) ? 1.0f : 0.0f;
}

float Gait::footPhase(int i, float done, const kin::Vec3 neutral[NUM_LEGS]) const {
  const float cycle = cycleTime(active_type_, params_.swing_s * cadence_);
  const float stance = dutyFactor(active_type_) * cycle;
  const Leg& l = legs_[i];
  const float wx = vx_ - wz_ * neutral[i].y, wy = vy_ + wz_ * neutral[i].x;
  const float w2 = wx * wx + wy * wy;
  float rest = (1.0f - clamp01(done)) * stance;
  if (w2 * cycle * cycle > 1.0f) {  // läuft (> 1 mm je Zyklus): Restzeit aus der Fußstellung
    rest = ((l.pos.x - neutral[i].x) * wx + (l.pos.y - neutral[i].y) * wy) / w2 + 0.5f * stance;
  }
  rest = rest < 0.0f ? 0.0f : (rest > stance ? stance : rest);
  return std::fmin(1.0f - rest / cycle, 1.0f - 1e-6f);
}

float Gait::reachTime(int i) const {
  if (min_radius_ < 0.0f) return 1e9f;
  const Leg& l = legs_[i];
  // Fuß relativ zur Coxa-Achse und seine Bewegung im Lauf-System: -(v + w x p)
  const float rx = l.pos.x - coxa_[i].x, ry = l.pos.y - coxa_[i].y;
  const float dx = wz_ * l.pos.y - vx_, dy = -vy_ - wz_ * l.pos.x;
  const float dd = dx * dx + dy * dy;
  if (dd < 1e-6f) return 1e9f;
  const float rd = rx * dx + ry * dy, r2 = rx * rx + ry * ry;
  const float m2 = min_radius_ * min_radius_, M2 = max_radius_ * max_radius_;
  if (r2 <= m2 || r2 >= M2) return 0.0f;
  float t = (-rd + std::sqrt(rd * rd + dd * (M2 - r2))) / dd;  // außen: |r + t*d| = M
  const float disc = rd * rd - dd * (r2 - m2);                   // innen: |r + t*d| = m
  if (rd < 0.0f && disc > 0.0f) t = std::fmin(t, (-rd - std::sqrt(disc)) / dd);
  return t;
}

void Gait::switchType(float cadence, const kin::Vec3 neutral[NUM_LEGS]) {
  const float sigma_old = SWING_FRACTION[static_cast<int>(active_type_)];
  const float cycle_old = cycleTime(active_type_, params_.swing_s * cadence_);
  from_type_ = active_type_;
  active_type_ = params_.type;
  cadence_ = cadence;
  converging_ = switching_ = true;
  const float sigma = SWING_FRACTION[static_cast<int>(active_type_)];

  // Phase je Bein in der neuen Gangart, passend zum Stand seines Fußes; ein laufender
  // Schwung endet in seinem Tempo am geplanten Aufsetzpunkt
  for (int i = 0; i < NUM_LEGS; ++i) {
    Leg& l = legs_[i];
    float p;
    if (l.swing) {
      if (l.dur <= 0.0f) {
        l.dur = sigma_old * cycle_old;
        l.half = 0.5f * (1.0f - sigma_old) * cycle_old;
      }
      p = l.u * sigma;
    } else if (l.wait) {
      p = 0.0f;
    } else {
      p = footPhase(i, (l.ph - sigma_old) / (1.0f - sigma_old), neutral);
    }
    l.off = frac(phase_ - p);
    l.ph = p;
  }
}

void Gait::update(float dt, const kin::Vec3 neutral[NUM_LEGS]) {
  if (dt <= 0.0f) return;
  if (dt > MAX_DT) dt = MAX_DT;
  if (min_radius_ < min_target_) min_radius_ = std::fmin(min_target_, min_radius_ + REACH_GLIDE * dt);
  if (max_radius_ > max_target_) max_radius_ = std::fmax(max_target_, max_radius_ - REACH_GLIDE * dt);
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
    converging_ = switching_ = false;
    const int t0 = static_cast<int>(active_type_);
    for (int i = 0; i < NUM_LEGS; ++i) {
      Leg& l = legs_[i];
      l.off = SWING_START[t0][i];
      l.ph = frac(-l.off);
      if (l.ph <= 0.0f) l.ph = 1.0f;  // Schwungbeginn mit dem ersten Schritt
      l.swing = l.wait = false;
      l.dur = 0.0f;
    }
  }

  // Gangwechsel im Lauf: umtakten, sobald Tempo und Reichweite zur neuen Gangart passen
  // (Klettern: erst im Stillstand, siehe computeVelocity)
  if (params_.type != active_type_ && !stopSwitch()) {
    const float cad = switchCadence(dt, neutral);
    if (cad > 0.0f) switchType(cad, neutral);
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

  const int t = static_cast<int>(active_type_);
  const float cycle = cycleTime(active_type_, params_.swing_s * cadence_);
  const float sigma = SWING_FRACTION[t];
  const float half_stance = 0.5f * (1.0f - sigma) * cycle;
  const float dp = dt / cycle;
  const float next_phase = phase_ + dp;
  if (next_phase >= 1.0f) starting_ = false;  // jedes Bein hatte sein Schwungfenster
  phase_ = frac(next_phase);

  // Nach dem Umtakten: Standbeine ins Muster führen (Phase etwas langsamer bzw.
  // schneller), nahe dran exakt übernehmen
  if (converging_) {
    // Das Muster gilt bis auf eine gemeinsame Lage: mittlere Abweichung herausnehmen
    float sx = 0.0f, sy = 0.0f;
    for (int i = 0; i < NUM_LEGS; ++i) {
      if (legs_[i].wait) continue;
      const float a = TWO_PI * (legs_[i].off - SWING_START[t][i]);
      sx += std::cos(a);
      sy += std::sin(a);
    }
    const float shift = sx * sx + sy * sy > 1e-6f ? std::atan2(sy, sx) / TWO_PI : 0.0f;
    if (std::fabs(shift) > 0.5f * PHASE_SNAP) {  // kleine Reste gleichen die Beine einzeln aus
      phase_ = frac(phase_ - shift);
      for (auto& l : legs_) l.off = frac(l.off - shift);
    }
  }
  float max_err = 0.0f;
  bool off_pattern = false;
  for (int i = 0; i < NUM_LEGS; ++i) {
    Leg& l = legs_[i];
    if (converging_ && !l.swing) {
      if (l.wait) {
        l.off = phase_;  // Phase wartet am Schwungbeginn
      } else {
        const float ahead = wrapHalf(SWING_START[t][i] - l.off);  // > 0: Bein ist voraus
        if (std::fabs(ahead) < PHASE_SNAP) l.off = SWING_START[t][i];
        else l.off = frac(l.off + std::fmax(-PHASE_FAST, std::fmin(PHASE_SLOW, PHASE_GAIN * ahead)) * dp);
      }
    }
    max_err = std::fmax(max_err, std::fabs(wrapHalf(SWING_START[t][i] - l.off)));
    off_pattern = off_pattern || l.off != SWING_START[t][i] || l.wait || l.dur > 0.0f;
  }
  phase_err_ = max_err;
  if (converging_ && !off_pattern) converging_ = switching_ = false;

  auto targetOf = [&](int i) {
    // Aufsetzpunkt: so weit vor der Neutralstellung, dass die folgende
    // Standphase symmetrisch um sie herum verläuft
    const float half = legs_[i].dur > 0.0f ? legs_[i].half : half_stance;
    return kin::Vec3{neutral[i].x + (vx_ - wz_ * neutral[i].y) * half,
                     neutral[i].y + (vy_ + wz_ * neutral[i].x) * half, 0.0f};
  };

  // 1. Schwungbeine: weiter bzw. aufsetzen (vor dem Abheben anderer Beine, damit an der
  //    Grenze nie zwei Gruppen gleichzeitig in der Luft sind)
  for (int i = 0; i < NUM_LEGS; ++i) {
    Leg& l = legs_[i];
    if (!l.swing) continue;
    const float p = frac(phase_ - l.off);
    bool land;
    if (l.dur > 0.0f) {
      l.u += dt / l.dur;
      land = l.u >= 1.0f - 1e-4f;
    } else {
      l.u = p / sigma;
      land = p >= sigma - PHASE_EPS;  // Fenster minimal kürzen (Rundung)
    }
    if (land) {
      // Kastenbahn: dort aufsetzen, wo der Fuß ist (folgt dem Aufsetzpunkt nur gleitend)
      l.pos = l.box ? kin::Vec3{l.path.x, l.path.y, 0.0f} : targetOf(i);
      l.swing = false;
      if (l.dur > 0.0f) {
        // Schwung aus der alten Gangart: Phase passend zum Aufsetzpunkt
        l.dur = 0.0f;
        const float pf = footPhase(i, 0.0f, neutral);
        l.off = frac(phase_ - pf);
        l.ph = pf;
      } else {
        l.ph = p;
      }
    }
  }
  // 2. Standbeine: am Schwungbeginn abheben, wenn nötig und kein Nachbarbein in der Luft ist.
  //    Würde ein Fuß vor seinem Schwung den Rand der Reichweite erreichen (z. B. nach
  //    einer Richtungsumkehr oder beim Anlaufen), setzt er vorher um – diese Beine zuerst.
  const float swing_dur = sigma * cycle;
  int order[NUM_LEGS];
  float urgency[NUM_LEGS];
  bool wrapped[NUM_LEGS];
  for (int i = 0; i < NUM_LEGS; ++i) {
    const Leg& l = legs_[i];
    const float p = frac(phase_ - l.off);
    wrapped[i] = !l.swing && !l.wait && p < l.ph - 0.5f;
    urgency[i] = 1e9f;
    // (nur wenn der Schritt etwas bringt: Fuß nicht schon am Aufsetzpunkt)
    if (moving && !l.swing && distXY(l.pos, targetOf(i)) > URGENT_MIN_STEP) {
      const float due = (l.wait || wrapped[i]) ? 0.0f : (1.0f - p) * cycle;  // bis zum geplanten Schwung
      const float rt = reachTime(i);
      if (rt < swing_dur && rt < due) urgency[i] = rt;
    }
    int k = i;
    for (; k > 0 && urgency[order[k - 1]] > urgency[i]; --k) order[k] = order[k - 1];
    order[k] = i;
  }
  for (int n = 0; n < NUM_LEGS; ++n) {
    const int i = order[n];
    Leg& l = legs_[i];
    if (l.swing) continue;
    bool lift = false;
    if (l.wait) {
      lift = liftAllowed(i);
    } else if (wrapped[i] && (moving || distXY(l.pos, neutral[i]) > SETTLE_TOL)) {
      lift = liftAllowed(i);
      if (!lift) {
        l.wait = true;  // sobald erlaubt; bis dahin wartet die Phase
        l.off = phase_;
        converging_ = true;
      }
    } else if (urgency[i] < 1e9f) {
      // vorzeitig umsetzen (bzw. sobald erlaubt), Phase beginnt neu
      lift = liftAllowed(i);
      l.wait = !lift;
      l.off = phase_;
      converging_ = true;
    }
    l.ph = frac(phase_ - l.off);
    if (lift) {
      l.wait = false;
      l.swing = true;
      l.dur = 0.0f;
      l.u = l.ph / sigma;
      l.h = 0.0f;
      l.box = active_type_ == Type::Climb;
      l.lift = l.box ? params_.climb_height : params_.step_height;
      l.path = l.pos;
    }
  }
  // Wartet ein Standbein auf einen Nachbarn in der Luft, darf sein Fuß den Rand der
  // Reichweite nicht erreichen, bevor der Nachbar aufsetzt: sonst den Körper abbremsen
  // (wirkt im nächsten Schritt; bis zum Stillstand, wenn der Fuß schon am Rand steht)
  float brake = 1.0f;
  for (int i = 0; i < NUM_LEGS; ++i) {
    const Leg& l = legs_[i];
    if (l.swing || !l.wait) continue;
    float free_in = 0.0f;  // Zeit, bis der letzte Nachbar aufsetzt
    for (int k = 0; k < 2; ++k) {
      const Leg& n = legs_[k ? NEIGHBOR_B[i] : NEIGHBOR_A[i]];
      if (n.swing) free_in = std::fmax(free_in, (1.0f - n.u) * (n.dur > 0.0f ? n.dur : swing_dur));
    }
    const float need = free_in + 2.0f * dt;
    const float rt = reachTime(i);
    if (rt < need) brake = std::fmin(brake, rt / need);
  }
  brake_ = brake;

  // 3. Schwungbahnen: Heben und Vorschwingen gleichzeitig, beide mit weichem Anfang und
  //    Ende (früh nach außen zu schwingen entlastet den Femur beim Abheben nah am Körper).
  //    Verschiebt sich der Aufsetzpunkt (Tempo ändert sich), führt der verbleibende Weg
  //    ohne Sprung dorthin.
  for (int i = 0; i < NUM_LEGS; ++i) {
    Leg& l = legs_[i];
    if (!l.swing) continue;
    const kin::Vec3 target = targetOf(i);
    const float h = l.box ? climbTravel(l.u) : smoothStep5(l.u);
    kin::Vec3& path = l.box ? l.path : l.pos;
    if (l.h < 1.0f) {
      const float k = (h - l.h) / (1.0f - l.h);
      path.x += (target.x - path.x) * k;
      path.y += (target.y - path.y) * k;
    } else {
      // Kastenbahn, schon vorgeschwungen: Ändert sich das Tempo, wandert der Aufsetzpunkt
      // (halbe Standphase ist beim Klettern lang) – gleitend folgen, nicht springen
      const float dx = target.x - path.x, dy = target.y - path.y;
      const float d = std::hypot(dx, dy), step = CLIMB_FOLLOW * dt;
      const float k = d > step ? step / d : 1.0f;
      path.x += dx * k;
      path.y += dy * k;
    }
    l.h = h;
    if (l.box) {
      // Kastenbahn, oben nach außen (Bogen nur auf die Ausgabe, nicht auf den Weg)
      const float rx = path.x - coxa_[i].x, ry = path.y - coxa_[i].y;
      const float r = std::hypot(rx, ry);
      const float rn = std::hypot(neutral[i].x - coxa_[i].x, neutral[i].y - coxa_[i].y);
      const float k = r > 1e-3f ? climbOut(l.u, r, rn) / r : 0.0f;
      l.pos = {path.x + rx * k, path.y + ry * k, l.lift * climbLift(l.u)};
    } else {
      l.pos.z = l.lift * liftProfile(l.u);
    }
    if (guard_d2_ > 0.0f) {
      // nicht näher an die Coxa-Achse, als das Knie in dieser Höhe zulässt
      const float dz = guard_h_ - l.pos.z;
      const float need = guard_d2_ - dz * dz;
      if (need > 0.0f) {
        const float r_min = guard_coxa_ + std::sqrt(need);
        const float rx = l.pos.x - coxa_[i].x, ry = l.pos.y - coxa_[i].y;
        const float r = std::hypot(rx, ry);
        if (r < r_min && r > 1e-3f) {
          l.pos.x = coxa_[i].x + rx * r_min / r;
          l.pos.y = coxa_[i].y + ry * r_min / r;
        }
      }
    }
  }

  if (!moving) {
    // Angekommen? Dann anhalten (kein Bein in der Luft, alle in Neutralstellung)
    for (int i = 0; i < NUM_LEGS; ++i) {
      if (legs_[i].swing || legs_[i].wait || distXY(legs_[i].pos, neutral[i]) > SETTLE_TOL) return;
    }
    idle_ = true;
    starting_ = true;
    converging_ = switching_ = false;
    phase_ = 0.0f;
    vx_ = vy_ = wz_ = 0.0f;
    for (int i = 0; i < NUM_LEGS; ++i) legs_[i].pos = {neutral[i].x, neutral[i].y, 0.0f};
    active_type_ = params_.type;
  }
}

}  // namespace gait
