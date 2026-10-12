#pragma once
// Lage (Nick/Roll) aus Beschleunigung und Drehrate – reine Mathematik ohne
// Arduino-Abhängigkeiten (Tests: test_host/attitude_test.cpp).
//
// Körpersystem wie in der Kinematik: x vorne, y links, z oben.
//   Nick (pitch) + = Nase hoch
//   Roll         + = rechte Seite runter (linke Seite hoch)
// – also dieselben Vorzeichen wie bei der Körperhaltung (kin::BodyPose).

#include <stdint.h>

namespace attitude {

struct V3 {
  float x, y, z;
};

// Einbaulage der Sensorplatine (GY-521): wohin der aufgedruckte X-Pfeil zeigt
// und ob die Bauteilseite nach unten zeigt (Platine kopfüber eingebaut)
enum class AxisDir : uint8_t { Forward = 0, Left = 1, Backward = 2, Right = 3 };
struct Mount {
  AxisDir x_dir = AxisDir::Forward;
  bool upside_down = false;
};

const char* axisDirName(AxisDir d);  // "vorne", "links", "hinten", "rechts"
bool axisDirFromName(const char* name, AxisDir& out);

// Sensor-Achsen -> Körper-Achsen
V3 toBody(const Mount& m, const V3& sensor);

// Lage allein aus der Beschleunigung (Grad). a = Messwert im Körpersystem; in
// Ruhe zeigt er nach oben (|a| = 1 g).
float accRollDeg(const V3& a);
float accPitchDeg(const V3& a);

// Komplementärfilter: Drehrate kurzfristig, Schwerkraft langfristig
class Filter {
 public:
  void reset(const V3& a_body);
  // gyro_dps = Drehrate im Körpersystem (rechte Hand um x, y, z)
  void update(const V3& a_body, const V3& gyro_dps, float dt, float k);
  float rollDeg() const { return roll_; }
  float pitchDeg() const { return pitch_; }

 private:
  float roll_ = 0.0f;
  float pitch_ = 0.0f;
};

// Mittelwert und Streuung einer Messreihe (Kalibrierung: steht der Roboter still?)
class Stats {
 public:
  void reset();
  void add(const V3& v);
  int count() const { return n_; }
  V3 mean() const;
  float maxStdDev() const;  // größte Standardabweichung der drei Achsen

 private:
  int n_ = 0;
  double sum_[3] = {0, 0, 0};
  double sq_[3] = {0, 0, 0};
};

}  // namespace attitude
