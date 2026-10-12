#include "attitude.h"

#include <cmath>
#include <cstring>

namespace attitude {
namespace {
constexpr float RAD2DEG = 57.2957795f;

V3 cross(const V3& a, const V3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
}  // namespace

const char* axisDirName(AxisDir d) {
  switch (d) {
    case AxisDir::Left:     return "links";
    case AxisDir::Backward: return "hinten";
    case AxisDir::Right:    return "rechts";
    default:                return "vorne";
  }
}

bool axisDirFromName(const char* name, AxisDir& out) {
  if (!name) return false;
  for (int i = 0; i < 4; ++i) {
    AxisDir d = static_cast<AxisDir>(i);
    if (std::strcmp(name, axisDirName(d)) == 0) {
      out = d;
      return true;
    }
  }
  return false;
}

V3 toBody(const Mount& m, const V3& s) {
  // Sensor-Achsen im Körpersystem: x nach Pfeil, z oben (kopfüber: unten), y = z × x
  static const V3 X_DIR[4] = {{1, 0, 0}, {0, 1, 0}, {-1, 0, 0}, {0, -1, 0}};
  const V3 ex = X_DIR[static_cast<int>(m.x_dir) & 3];
  const V3 ez = {0.0f, 0.0f, m.upside_down ? -1.0f : 1.0f};
  const V3 ey = cross(ez, ex);
  return {s.x * ex.x + s.y * ey.x + s.z * ez.x,
          s.x * ex.y + s.y * ey.y + s.z * ez.y,
          s.x * ex.z + s.y * ey.z + s.z * ez.z};
}

float accRollDeg(const V3& a) { return std::atan2(a.y, a.z) * RAD2DEG; }

float accPitchDeg(const V3& a) {
  return std::atan2(a.x, std::sqrt(a.y * a.y + a.z * a.z)) * RAD2DEG;
}

void Filter::reset(const V3& a_body) {
  roll_ = accRollDeg(a_body);
  pitch_ = accPitchDeg(a_body);
}

void Filter::update(const V3& a, const V3& g, float dt, float k) {
  // Roll = Drehung um +x (hebt die linke Seite), Nase hoch = Drehung um -y
  roll_ = k * (roll_ + g.x * dt) + (1.0f - k) * accRollDeg(a);
  pitch_ = k * (pitch_ - g.y * dt) + (1.0f - k) * accPitchDeg(a);
}

void Stats::reset() {
  n_ = 0;
  for (int i = 0; i < 3; ++i) sum_[i] = sq_[i] = 0.0;
}

void Stats::add(const V3& v) {
  const float c[3] = {v.x, v.y, v.z};
  for (int i = 0; i < 3; ++i) {
    sum_[i] += c[i];
    sq_[i] += static_cast<double>(c[i]) * c[i];
  }
  ++n_;
}

V3 Stats::mean() const {
  if (n_ == 0) return {0, 0, 0};
  return {static_cast<float>(sum_[0] / n_), static_cast<float>(sum_[1] / n_),
          static_cast<float>(sum_[2] / n_)};
}

float Stats::maxStdDev() const {
  if (n_ < 2) return 0.0f;
  double worst = 0.0;
  for (int i = 0; i < 3; ++i) {
    double m = sum_[i] / n_;
    double var = sq_[i] / n_ - m * m;
    if (var > worst) worst = var;
  }
  return static_cast<float>(std::sqrt(worst));
}

}  // namespace attitude
