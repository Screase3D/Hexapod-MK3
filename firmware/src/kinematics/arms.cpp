#include "arms.h"

#include <cmath>

#include "gait.h"

namespace arms {
namespace {
constexpr float TWO_PI = 6.28318531f;

float clamp1(float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

float approach(float cur, float target, float step) {
  float d = target - cur;
  if (d > step) return cur + step;
  if (d < -step) return cur - step;
  return target;
}

kin::Vec3 scale(const kin::Vec3& v, float k) { return {v.x * k, v.y * k, v.z * k}; }
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
  for (auto& c : cur_) c = {0, 0, 0};
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
    case State::Raise:
    case State::Up:
      state_ = State::Lower;
      t_ = 0.0f;
      from_[0] = cur_[0];
      from_[1] = cur_[1];
      return true;
    default:
      return state_ != State::Down;  // senkt sich schon ab
  }
}

void Controller::setControl(float lx, float lz, float rx, float rz) {
  ctrl_[0] = clamp1(lx);
  ctrl_[1] = clamp1(lz);
  ctrl_[2] = clamp1(rx);
  ctrl_[3] = clamp1(rz);
}

bool Controller::wave() {
  if (state_ != State::Up) return false;
  wave_t_ = 0.0f;
  return true;
}

Output Controller::update(float dt, bool gait_idle) {
  t_ += dt;
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
        from_[0] = from_[1] = {0, 0, 0};
      }
      break;
    }

    case State::Raise: {
      // Vorderfüße lösen sich (Versatz 0 = dort, wo die Gangart sie hält) und
      // schwingen in die Bereitstellung
      float s = gait::smoothStep5(t_ / cfg_.raise_s);
      for (int i = 0; i < 2; ++i) cur_[i] = scale(cfg_.ready, s);
      out_.blend = 1.0f;
      if (t_ >= cfg_.raise_s) {
        state_ = State::Up;
        t_ = 0.0f;
      }
      break;
    }

    case State::Up: {
      kin::Vec3 target[2];
      for (int i = 0; i < 2; ++i) {
        const float cx = ctrl_[i * 2];
        target[i] = {cfg_.ready.x + cx * (cx > 0.0f ? cfg_.range_fwd : cfg_.range_back), cfg_.ready.y,
                     cfg_.ready.z + ctrl_[i * 2 + 1] * cfg_.range_z};
      }
      if (wave_t_ >= 0.0f) {
        // rechter Fuß hoch und vor, winkt seitlich
        wave_t_ += dt;
        target[1] = {cfg_.ready.x + 10.0f, 18.0f * std::sin(TWO_PI * 1.25f * wave_t_), cfg_.ready.z + 25.0f};
        if (wave_t_ >= cfg_.wave_s) wave_t_ = -1.0f;
      }
      const float step = cfg_.follow_mm_s * dt;
      for (int i = 0; i < 2; ++i) {
        cur_[i].x = approach(cur_[i].x, target[i].x, step);
        cur_[i].y = approach(cur_[i].y, target[i].y, step);
        cur_[i].z = approach(cur_[i].z, target[i].z, step);
      }
      out_.blend = 1.0f;
      break;
    }

    case State::Lower: {
      // aus der aktuellen Stellung zurück auf den Boden (Neutralpunkt)
      float s = gait::smoothStep5(t_ / cfg_.raise_s);
      for (int i = 0; i < 2; ++i) cur_[i] = scale(from_[i], 1.0f - s);
      out_.blend = 1.0f;
      if (t_ >= cfg_.raise_s) {
        state_ = State::ShiftForward;
        t_ = 0.0f;
        shift_from_ = out_.body_dx;
        cur_[0] = cur_[1] = {0, 0, 0};
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
  out_.foot[0] = cur_[0];
  out_.foot[1] = cur_[1];
  return out_;
}

}  // namespace arms
