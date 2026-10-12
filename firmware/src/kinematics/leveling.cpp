#include "leveling.h"

#include <cmath>

namespace leveling {
namespace {
float approachZero(float v, float step) {
  if (v > step) return v - step;
  if (v < -step) return v + step;
  return 0.0f;
}
}  // namespace

void Controller::reset() {
  roll_ = pitch_ = 0.0f;
  sat_roll_ = sat_pitch_ = 0.0f;
}

void Controller::axis(float dt, float err, float& corr, float& sat_t) {
  float e = std::fabs(err) > cfg_.deadband_deg ? err - std::copysign(cfg_.deadband_deg, err) : 0.0f;
  corr += cfg_.gain * e * dt;
  if (corr > cfg_.max_deg) corr = cfg_.max_deg;
  if (corr < -cfg_.max_deg) corr = -cfg_.max_deg;
  // Falsche Richtung: Korrektur am Anschlag, Abweichung bleibt groß (richtig
  // herum wäre sie bei voller Korrektur um max_deg kleiner als die Schräglage)
  bool saturated = std::fabs(corr) >= cfg_.max_deg - 1e-3f && std::fabs(err) > 0.8f * cfg_.max_deg;
  sat_t = saturated ? sat_t + dt : 0.0f;
  if (sat_t > cfg_.fault_s) fault_ = true;
}

void Controller::update(float dt, bool active, float meas_roll, float meas_pitch,
                        float target_roll, float target_pitch) {
  if (!active || fault_) {
    roll_ = approachZero(roll_, cfg_.release_deg_s * dt);
    pitch_ = approachZero(pitch_, cfg_.release_deg_s * dt);
    sat_roll_ = sat_pitch_ = 0.0f;
    return;
  }
  axis(dt, target_roll - meas_roll, roll_, sat_roll_);
  axis(dt, target_pitch - meas_pitch, pitch_, sat_pitch_);
}

}  // namespace leveling
