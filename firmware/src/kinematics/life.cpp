#include "life.h"

#include <cmath>

#include "gait.h"

namespace life {
namespace {
constexpr float TWO_PI = 6.28318531f;
constexpr float RAD2DEG = 57.2957795f;

// Stimmung
constexpr float BORED_FRACTION = 0.4f;  // gelangweilt nach 40 % der Einschlafzeit
constexpr float ACT_MIN_S = 18.0f;      // Pause zwischen zwei Langeweile-Aktionen
constexpr float ACT_MAX_S = 35.0f;
constexpr float LAST_YAWN_S = 5.0f;     // letztes Gähnen (ca. 4,7 s), dann hinsetzen

// Atmen
constexpr float BREATH_S = 4.2f;
constexpr float BREATH_MM = 2.0f;

// Hochheben
// (Schwellen bei mittlerer Empfindlichkeit; gering x1,6, hoch x0,6)
constexpr float LIFT_ARM_S = 0.4f;      // erst so lange nach dem Stehenbleiben scharf
constexpr float LIFT_ACC_G = 0.10f;     // Abweichung von 1 g
constexpr float LIFT_GYRO_DPS = 22.0f;  // Drehrate im Stand ...
constexpr float LIFT_GYRO_MOVING = 55.0f;  // ... bzw. während eigener Bewegungen
constexpr float LIFT_TILT_DEG = 6.0f;   // Neigung weicht von der gewollten ab
constexpr float LIFT_BASE_S = 2.0f;     // Grundlinie der Neigung folgt so langsam
constexpr float LIFT_SCORE = 0.045f;    // 2–3 Takte über der Schwelle (ca. 40–60 ms)
constexpr float LIFT_DECAY_S = 0.2f;

// Zappeln
constexpr float FLAIL_S = 2.2f;
constexpr float FLAIL_HZ = 2.6f;
constexpr float FLAIL_LIFT_MM = 24.0f;
constexpr float FLAIL_KICK_MM = 10.0f;
constexpr float FLAIL_FADE_S = 0.25f;

// Laufen
constexpr float LEAN_K = 0.012f;        // mm Auslenkung je mm/s² Beschleunigung
constexpr float LEAN_MAX_MM = 7.0f;
constexpr float LEAN_HZ = 1.3f;         // Feder: Eigenfrequenz ...
constexpr float LEAN_DAMP = 0.45f;      // ... und Dämpfung (schwingt leicht nach)
constexpr float LOOK_TURN_DEG = 9.0f;   // Blick in die Kurve bei voller Drehrate
constexpr float LOOK_SIDE_MAX = 6.0f;   // Blick in Laufrichtung beim Seitwärtslaufen
constexpr float LEAN_PITCH_DEG = 2.0f;  // vorwärts: Nase etwas runter
constexpr float SWAY_ROLL_DEG = 1.6f;   // Hüftschwung
constexpr float SWAY_BOB_MM = 1.2f;
constexpr float STOP_LOOK_MIN_S = 2.5f; // "hat angehalten" erst nach so langem Laufen

float approach(float cur, float target, float step) {
  float d = target - cur;
  if (d > step) return cur + step;
  if (d < -step) return cur - step;
  return target;
}
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float lowpass(float cur, float target, float dt, float tau) { return cur + (target - cur) * clampf(dt / tau, 0.0f, 1.0f); }
}  // namespace

const char* moodName(Mood m) {
  switch (m) {
    case Mood::Bored:  return "bored";
    case Mood::Asleep: return "asleep";
    default:           return "awake";
  }
}

// ---- Stimmung

float Moods::rnd(float a, float b) {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return a + (b - a) * ((rng_ >> 8) * (1.0f / 16777216.0f));
}

MoodOut Moods::update(const MoodIn& in, float sleep_s) {
  MoodOut o;
  if (!in.enabled) {
    mood_ = Mood::Awake;
    idle_s_ = 0.0f;
    sit_in_ = -1.0f;
    stretch_ = wake_ = false;
    return o;
  }
  if (in.activity) {
    idle_s_ = 0.0f;
    sit_in_ = -1.0f;
    // schläft (sitzt bzw. setzt sich gerade): Bewegungswunsch weckt ihn auf – aufstehen,
    // danach strecken. Andere Eingaben (z. B. "hinsetzen") wecken nur.
    if (mood_ == Mood::Asleep && in.move) {
      wake_ = stretch_ = true;
    } else if (!in.move) {
      wake_ = false;
    }
    mood_ = Mood::Awake;
  }
  if (!in.standing) {
    if (mood_ != Mood::Asleep) idle_s_ = 0.0f;  // sitzt von sich aus: keine Langeweile
    o.stand = wake_;
    return o;
  }
  wake_ = false;
  if (mood_ == Mood::Asleep && sit_in_ < 0.0f) mood_ = Mood::Awake;  // anders aufgestanden
  if (stretch_ && in.still) {
    stretch_ = false;
    o.play = alive::Action::Stretch;
    return o;
  }
  if (sit_in_ >= 0.0f) {
    sit_in_ -= in.dt;
    if (sit_in_ < 0.0f) {
      o.sit = true;
      mood_ = Mood::Asleep;
      idle_s_ = 0.0f;
    }
    return o;
  }
  if (in.still) idle_s_ += in.dt;
  if (mood_ == Mood::Awake && idle_s_ >= BORED_FRACTION * sleep_s) {
    mood_ = Mood::Bored;
    if (in.still) o.play = alive::Action::Yawn;
    next_act_ = rnd(ACT_MIN_S, ACT_MAX_S);
    return o;
  }
  if (mood_ == Mood::Bored && in.still) {
    if (idle_s_ >= sleep_s) {
      o.play = alive::Action::Yawn;  // noch einmal gähnen, dann hinsetzen
      sit_in_ = LAST_YAWN_S;
      return o;
    }
    next_act_ -= in.dt;
    if (next_act_ <= 0.0f) {
      const float r = rnd(0.0f, 1.0f);
      o.play = r < 0.4f ? alive::Action::Yawn : (r < 0.7f ? alive::Action::Sigh : alive::Action::Look);
      next_act_ = rnd(ACT_MIN_S, ACT_MAX_S);
    }
  }
  return o;
}

// ---- Atmen

float Breath::update(float dt, bool on) {
  level_ = approach(level_, on ? 1.0f : 0.0f, dt);
  t_ += dt;
  if (t_ > BREATH_S) t_ -= BREATH_S;
  wave_ = std::sin(TWO_PI * t_ / BREATH_S);
  return BREATH_MM * wave_ * gait::smoothStep5(level_);
}

// ---- Hochheben

bool LiftDetector::update(const LiftIn& in) {
  const float dt = in.dt;
  if (!in.armed) {
    reset();
    return false;
  }
  if (armed_s_ <= 0.0f) {  // gerade scharf geworden: Grundlinie = jetzt
    base_[0] = in.tilt_err[0];
    base_[1] = in.tilt_err[1];
  }
  armed_s_ += dt;
  const float k = in.sens >= 3 ? 0.6f : (in.sens <= 1 ? 1.6f : 1.0f);
  const float dev = std::hypot(in.tilt_err[0] - base_[0], in.tilt_err[1] - base_[1]);
  const float bk = clampf(dt / LIFT_BASE_S, 0.0f, 1.0f);
  base_[0] += (in.tilt_err[0] - base_[0]) * bk;
  base_[1] += (in.tilt_err[1] - base_[1]) * bk;
  if (armed_s_ < LIFT_ARM_S) {
    score_ = 0.0f;
    return false;
  }
  const float a = std::sqrt(in.acc[0] * in.acc[0] + in.acc[1] * in.acc[1] + in.acc[2] * in.acc[2]);
  const float w = std::sqrt(in.gyro[0] * in.gyro[0] + in.gyro[1] * in.gyro[1] + in.gyro[2] * in.gyro[2]);
  const float d = std::fabs(a - 1.0f);
  const float th_a = LIFT_ACC_G * k;
  const float th_w = (in.moving ? LIFT_GYRO_MOVING : LIFT_GYRO_DPS) * k;
  const float th_t = LIFT_TILT_DEG * k;
  score_ *= std::exp(-dt / LIFT_DECAY_S);
  // je Takt höchstens das Doppelte: ein einzelner Stoß reicht nicht, auch wenn er stark ist
  if (d > th_a) score_ += dt * clampf(d / th_a, 0.0f, 2.0f);
  if (w > th_w) score_ += dt * 0.8f * clampf(w / th_w, 0.0f, 2.0f);
  if (dev > th_t) score_ += dt * clampf(dev / th_t, 0.0f, 2.0f);
  if (score_ >= LIFT_SCORE) {
    reset();
    return true;
  }
  return false;
}

// ---- Zappeln

void Flail::start() {
  if (t_ < 0.0f) {
    t_ = 0.0f;
    level_ = 0.0f;
  }
  stop_ = false;
}

alive::Output Flail::update(float dt) {
  alive::Output o;
  if (t_ < 0.0f) return o;
  t_ += dt;
  const bool ending = stop_ || t_ > FLAIL_S;
  level_ = approach(level_, ending ? 0.0f : 1.0f, dt / FLAIL_FADE_S);
  if (ending && level_ <= 0.0f) {
    reset();
    stop_ = false;
    return o;
  }
  const float g = gait::smoothStep5(level_);
  const float ph = TWO_PI * FLAIL_HZ * t_;
  const float s = std::sin(ph);
  for (int leg = 0; leg < 6; ++leg) {
    // Dreiergruppen 0/2/4 und 1/3/5 (wie beim Tripod): nie beide zugleich in der Luft
    const float up = leg % 2 == 0 ? (s > 0.0f ? s : 0.0f) : (s < 0.0f ? -s : 0.0f);
    o.foot[leg] = {FLAIL_KICK_MM * std::sin(2.0f * ph + leg * 1.3f) * up * g, 0.0f, FLAIL_LIFT_MM * up * g};
  }
  return o;
}

// ---- Laufen mit Charakter

void WalkFx::reset() { *this = WalkFx(); }

gestures::Offset WalkFx::update(const WalkIn& in) {
  gestures::Offset o;
  just_stopped_ = false;
  const float dt = in.dt;
  if (dt <= 0.0f) return o;
  const float v[2] = {in.vx, in.vy};
  if (!primed_) {
    last_v_[0] = v[0];
    last_v_[1] = v[1];
    primed_ = true;
  }
  // Trägheit: Körper bleibt beim Beschleunigen zurück und schwingt beim Anhalten nach
  const float w = TWO_PI * LEAN_HZ;
  for (int i = 0; i < 2; ++i) {
    const float a = (v[i] - last_v_[i]) / dt;
    last_v_[i] = v[i];
    acc_f_[i] = lowpass(acc_f_[i], a, dt, 0.12f);
    const float target = clampf(-LEAN_K * acc_f_[i], -LEAN_MAX_MM, LEAN_MAX_MM);
    lean_v_[i] += (w * w * (target - lean_[i]) - 2.0f * LEAN_DAMP * w * lean_v_[i]) * dt;
    lean_[i] = clampf(lean_[i] + lean_v_[i] * dt, -LEAN_MAX_MM, LEAN_MAX_MM);
  }
  o.x = lean_[0];
  o.y = lean_[1];

  // Blick voraus: in die Kurve, beim Seitwärtslaufen etwas in Laufrichtung
  const float speed = std::sqrt(in.vx * in.vx + in.vy * in.vy);
  const float turn = clampf(in.wz / (in.max_turn > 1.0f ? in.max_turn : 1.0f), -1.0f, 1.0f);
  float side = 0.0f;
  if (speed > 15.0f) {
    const float ax = std::fabs(in.vx) > 30.0f ? std::fabs(in.vx) : 30.0f;
    side = clampf(0.3f * std::atan2(in.vy, ax) * RAD2DEG, -LOOK_SIDE_MAX, LOOK_SIDE_MAX);
  }
  yaw_ = lowpass(yaw_, LOOK_TURN_DEG * turn + side, dt, 0.35f);
  const float maxv = in.max_speed > 1.0f ? in.max_speed : 1.0f;
  pitch_ = lowpass(pitch_, -LEAN_PITCH_DEG * clampf(in.vx / maxv, -1.0f, 1.0f), dt, 0.4f);
  o.yaw = yaw_;
  o.pitch = pitch_;

  // Hüftschwung im Schritttakt, stärker mit dem Tempo
  const float frac = in.on ? clampf(speed / maxv + 0.5f * std::fabs(turn), 0.0f, 1.0f) : 0.0f;
  sway_ = lowpass(sway_, frac, dt, 0.4f);
  o.roll = SWAY_ROLL_DEG * sway_ * std::sin(TWO_PI * in.phase);
  o.z = SWAY_BOB_MM * sway_ * std::cos(2.0f * TWO_PI * in.phase);

  if (in.on) walk_s_ += dt;
  if (was_on_ && !in.on) {
    just_stopped_ = walk_s_ >= STOP_LOOK_MIN_S;
    walk_s_ = 0.0f;
  }
  was_on_ = in.on;
  return o;
}

float gaze(float wz_norm, float vy_norm, float yaw_deg) {
  return clampf(wz_norm + 0.6f * vy_norm + yaw_deg / 12.0f, -1.0f, 1.0f);
}

}  // namespace life
