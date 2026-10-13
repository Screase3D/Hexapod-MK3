#include "arms.h"

#include <cmath>

#include "gait.h"

namespace arms {
namespace {
constexpr float TWO_PI = 6.28318531f;
constexpr float DEG2RAD = 0.0174532925f;
constexpr float R_STEP = 3.0f;      // Suche nach einem erreichbaren Abstand: Schrittweite ...
constexpr int R_STEPS = 20;         // ... und Anzahl je Richtung (±60 mm)
constexpr float RETURN_MAX_S = 4.0f;  // Absetzen: spätestens dann senkrecht runter
constexpr float WAVE_HZ = 1.0f;
constexpr float WAVE_AMP = 0.25f;   // rad (±14°)
constexpr float WAVE_H = 130.0f;    // Fußhöhe beim Winken

float clamp1(float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

float approach(float cur, float target, float step) {
  float d = target - cur;
  if (d > step) return cur + step;
  if (d < -step) return cur - step;
  return target;
}

// eine Achse weich auf ihr Ziel zu (Weg d, Geschwindigkeit v): liefert den Schritt
float stepAxis(float& v, float d, float vmax, float amax, float dt) {
  float vdes = std::sqrt(2.0f * amax * std::fabs(d));
  if (vdes > vmax) vdes = vmax;
  if (d < 0.0f) vdes = -vdes;
  v = approach(v, vdes, amax * dt);
  float s = v * dt;
  if (std::fabs(s) >= std::fabs(d) || std::fabs(d) < 0.05f) {
    v = 0.0f;
    return d;
  }
  return s;
}
}  // namespace

const char* stateName(State s) {
  switch (s) {
    case State::ShiftBack:    return "shift_back";
    case State::Raise:        return "raise";
    case State::Up:           return "up";
    case State::Lower:        return "lower";
    case State::ShiftForward: return "shift_forward";
    default:                  return "down";
  }
}

void Controller::reset() {
  state_ = State::Down;
  t_ = 0.0f;
  wave_t_ = -1.0f;
  descending_ = false;
  limited_ = false;
  for (auto& c : cur_) c = Polar();
  for (auto& a : ax_) a[0] = a[1] = Axis();
  for (float& c : ctrl_) c = 0.0f;
  out_ = Output();
}

bool Controller::requestUp() {
  if (state_ != State::Down) return state_ == State::Up || state_ == State::Raise || state_ == State::ShiftBack;
  state_ = State::ShiftBack;
  t_ = 0.0f;
  shift_from_ = out_.body_dx;
  return true;
}

bool Controller::requestDown() {
  wave_t_ = -1.0f;
  switch (state_) {
    case State::ShiftBack:  // Füße noch am Boden: Gewicht direkt zurück
      state_ = State::ShiftForward;
      t_ = 0.0f;
      shift_from_ = out_.body_dx;
      return true;
    case State::Raise:  // Füße noch senkrecht über dem Neutralpunkt: gleich runter
    case State::Up: {
      const bool raising = state_ == State::Raise;
      state_ = State::Lower;
      t_ = 0.0f;
      descending_ = false;
      for (int k = 0; k < 2; ++k) ax_[k][0] = ax_[k][1] = Axis();
      if (raising) startDescent();
      return true;
    }
    default:
      return state_ != State::Down;  // senkt sich schon ab
  }
}

void Controller::setControl(float lx, float lz, float rx, float rz, float reach) {
  ctrl_[0] = clamp1(lx);
  ctrl_[1] = clamp1(lz);
  ctrl_[2] = clamp1(rx);
  ctrl_[3] = clamp1(rz);
  ctrl_[4] = clamp1(reach);
}

bool Controller::wave() {
  if (state_ != State::Up) return false;
  wave_t_ = 0.0f;
  return true;
}

Controller::Polar Controller::groundPolar(const Frame& f, int k) const {
  Polar p;
  const float dx = f.ground[k].x - f.coxa[k].x, dy = f.ground[k].y - f.coxa[k].y;
  p.phi = std::atan2(dy, dx);
  p.r = std::sqrt(dx * dx + dy * dy);
  p.h = 0.0f;
  return p;
}

kin::Vec3 Controller::toFoot(const Frame& f, int k, const Polar& p) const {
  return {f.coxa[k].x + p.r * std::cos(p.phi), f.coxa[k].y + p.r * std::sin(p.phi), p.h};
}

bool Controller::reach(const Frame& f, int k, const Polar& p) const {
  return !f.reachable || f.reachable(f.ctx, k, toFoot(f, k, p));
}

bool Controller::follow(const Frame& f, int k, const Polar& target, float dt) {
  Polar& c = cur_[k];
  const float r_ref = c.r > 40.0f ? c.r : 40.0f;  // Schwenken als Bogen (mm) auf dem aktuellen Abstand
  const float d_arc = (target.phi - c.phi) * r_ref;
  const float d_h = target.h - c.h;
  const float s_arc = stepAxis(ax_[k][0].v, d_arc, cfg_.speed_mm_s, cfg_.accel_mm_s2, dt);
  const float s_h = stepAxis(ax_[k][1].v, d_h, cfg_.speed_mm_s, cfg_.accel_mm_s2, dt);
  const float r_step = 1.25f * cfg_.speed_mm_s * dt;

  // nächster erreichbarer Abstand zur Coxa (möglichst der gewünschte)
  auto nearestR = [&](Polar p, float& r_out) {
    for (int i = 0; i <= 2 * R_STEPS; ++i) {
      const int n = (i + 1) / 2;
      p.r = target.r + (i % 2 ? n : -n) * R_STEP;
      if (p.r < 20.0f) continue;
      if (reach(f, k, p)) {
        r_out = p.r;
        return true;
      }
    }
    return false;
  };
  // Schritt mit Anteil qa/qh der Schwenk-/Hebebewegung; der Abstand folgt begrenzt schnell
  auto tryStep = [&](float qa, float qh, Polar& out) {
    Polar n = c;
    n.phi += qa * s_arc / r_ref;
    n.h += qh * s_h;
    float r;
    if (!nearestR(n, r)) return false;
    n.r = approach(c.r, r, r_step);
    if (!reach(f, k, n)) return false;
    out = n;
    return true;
  };

  // voller Schritt; kommt der Abstand nicht nach oder ist der Rand erreicht: langsamer
  // bzw. nur die Achse, die noch geht
  static constexpr float TRIES[][2] = {{1, 1}, {0.5f, 0.5f}, {0.25f, 0.25f}, {0, 1}, {1, 0}, {0, 0}};
  for (const auto& q : TRIES) {
    Polar n;
    if (!tryStep(q[0], q[1], n)) continue;
    ax_[k][0].v *= q[0];
    ax_[k][1].v *= q[1];
    c = n;
    return std::fabs(d_arc) < 0.5f && std::fabs(d_h) < 0.5f && std::fabs(c.r - target.r) < 0.5f &&
           ax_[k][0].v == 0.0f && ax_[k][1].v == 0.0f;
  }
  // Notfall (z. B. Haltung geändert): direkt auf einen erreichbaren Abstand
  float r;
  if (nearestR(c, r)) c.r = r;
  ax_[k][0].v = ax_[k][1].v = 0.0f;
  return false;
}

float Controller::homeHeight(const Frame& f, int k) const {
  // höchster erreichbarer Punkt bis zur Bereitstellung senkrecht über dem Neutralpunkt
  Polar p = groundPolar(f, k);
  for (p.h = cfg_.ready_h; p.h > 0.0f; p.h -= 5.0f) {
    if (reach(f, k, p)) return p.h;
  }
  return 0.0f;
}

void Controller::startDescent() {
  descending_ = true;
  t_ = 0.0f;
  for (int k = 0; k < 2; ++k) {
    lower_from_[k] = cur_[k];
    ax_[k][0] = ax_[k][1] = Axis();
  }
}

Output Controller::update(float dt, bool gait_idle, const Frame& frame) {
  t_ += dt;
  limited_ = false;
  switch (state_) {
    case State::Down:
      out_ = Output();
      break;

    case State::ShiftBack: {
      // Körper nach hinten, Mittelbeine nach vorne (Schritte der Gangart)
      float s = gait::smoothStep5(t_ / cfg_.shift_s);
      out_.body_dx = shift_from_ + (-cfg_.cog_shift - shift_from_) * s;
      out_.mid_dx = cfg_.mid_forward;
      out_.blend = 0.0f;
      if (t_ >= cfg_.shift_s && gait_idle) {
        state_ = State::Raise;
        t_ = 0.0f;
        for (int k = 0; k < 2; ++k) {
          cur_[k] = groundPolar(frame, k);
          ax_[k][0] = ax_[k][1] = Axis();
        }
      }
      break;
    }

    case State::Raise: {
      // Vorderfüße senkrecht nach oben in die Bereitstellung (so weit erreichbar)
      const float h = cfg_.ready_h * gait::smoothStep5(t_ / cfg_.raise_s);
      for (int k = 0; k < 2; ++k) {
        Polar p = groundPolar(frame, k);
        p.h = h;
        if (reach(frame, k, p) || h < cur_[k].h) cur_[k] = p;
      }
      out_.blend = 1.0f;
      if (t_ >= cfg_.raise_s) {
        state_ = State::Up;
        t_ = 0.0f;
      }
      break;
    }

    case State::Up: {
      if (wave_t_ >= 0.0f) {
        wave_t_ += dt;
        if (wave_t_ >= cfg_.wave_s) wave_t_ = -1.0f;
      }
      for (int k = 0; k < 2; ++k) {
        const Polar g = groundPolar(frame, k);
        const float side = k == 0 ? 1.0f : -1.0f;  // links: nach innen = Winkel kleiner
        const float u = ctrl_[k * 2], z = ctrl_[k * 2 + 1], rc = ctrl_[4];
        Polar t;
        t.phi = g.phi - side * u * cfg_.sweep_deg * DEG2RAD;
        t.h = cfg_.ready_h + z * (z > 0.0f ? cfg_.up : cfg_.down);
        t.r = g.r + rc * (rc > 0.0f ? cfg_.reach_out : cfg_.reach_in);
        if (k == 1 && wave_t_ >= 0.0f) {
          // rechter Fuß hoch, schwenkt seitlich hin und her
          t.phi = g.phi + 0.1f + WAVE_AMP * std::sin(TWO_PI * WAVE_HZ * wave_t_);
          t.h = WAVE_H;
          t.r = g.r - 10.0f;
        }
        if (!reach(frame, k, t)) limited_ = true;
        follow(frame, k, t, dt);
      }
      out_.blend = 1.0f;
      break;
    }

    case State::Lower: {
      out_.blend = 1.0f;
      if (!descending_) {
        // zurück in die Bereitstellung über dem (aktuellen) Neutralpunkt
        bool home = true;
        for (int k = 0; k < 2; ++k) {
          Polar t = groundPolar(frame, k);
          t.h = homeHeight(frame, k);
          home = follow(frame, k, t, dt) && home;
        }
        if (home || t_ >= RETURN_MAX_S) startDescent();
        break;
      }
      // auf den Boden (von der Bereitstellung aus senkrecht)
      const float s = gait::smoothStep5(t_ / cfg_.raise_s);
      for (int k = 0; k < 2; ++k) {
        const Polar g = groundPolar(frame, k);
        const Polar& a = lower_from_[k];
        cur_[k] = {a.phi + (g.phi - a.phi) * s, a.r + (g.r - a.r) * s, a.h * (1.0f - s)};
      }
      if (t_ >= cfg_.raise_s) {
        state_ = State::ShiftForward;
        t_ = 0.0f;
        descending_ = false;
        shift_from_ = out_.body_dx;
        out_.blend = 0.0f;
      }
      break;
    }

    case State::ShiftForward: {
      float s = gait::smoothStep5(t_ / cfg_.shift_s);
      out_.body_dx = shift_from_ * (1.0f - s);
      out_.mid_dx = 0.0f;
      out_.blend = 0.0f;
      if (t_ >= cfg_.shift_s && gait_idle) {
        state_ = State::Down;
        t_ = 0.0f;
        out_ = Output();
      }
      break;
    }
  }
  for (int k = 0; k < 2; ++k) out_.foot[k] = out_.blend > 0.0f ? toFoot(frame, k, cur_[k]) : frame.ground[k];
  return out_;
}

}  // namespace arms
