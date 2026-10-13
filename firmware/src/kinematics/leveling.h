#pragma once
// Lageausgleich: hält den Körper mit Hilfe des Lagesensors waagerecht (bzw. auf
// der eingestellten Neigung), auch auf schrägem Untergrund – reine Mathematik
// (Tests: test_host/attitude_test.cpp).
//
// I-Regler je Achse: Die Korrektur wächst mit der Abweichung Soll - Ist und
// wird zur befohlenen Neigung addiert. Im Gleichgewicht gleicht sie gerade die
// Schräglage des Untergrunds aus. Die Korrektur ändert sich höchstens mit
// max_rate_deg_s – schneller kann der Körper ihr nicht folgen; ohne Grenze liefe
// der Regler bei großen Abweichungen voraus und schwänge über.
//
// Sicherheit: Dreht der Lagesensor falsch herum (Einbaulage falsch eingestellt),
// verstärkt der Regler die Schräglage, statt sie auszugleichen. Läuft eine Achse
// deshalb längere Zeit mit großer Abweichung in der Begrenzung, schaltet er sich
// mit fault() ab und baut die Korrektur ab.

namespace leveling {

struct Config {
  float gain = 4.0f;           // 1/s: Anteil der Abweichung, der je Sekunde ausgeglichen wird
  float max_rate_deg_s = 30.0f; // schnellste Änderung der Korrektur (Haltung folgt mit 45 °/s)
  float max_deg = 15.0f;       // größte Korrektur je Achse
  float deadband_deg = 0.5f;   // kleinere Abweichungen ignorieren (kein Zittern)
  float release_deg_s = 10.0f; // Abbau der Korrektur, wenn nicht aktiv
  float fault_s = 2.0f;        // so lange in der Begrenzung bei großer Abweichung -> Fehler
};

class Controller {
 public:
  explicit Controller(const Config& cfg = Config()) : cfg_(cfg) {}

  void reset();
  // Regelgeschwindigkeit (1/s), z. B. beim Laufen kleiner (Schritte schaukeln den Körper)
  void setGain(float gain) { cfg_.gain = gain; }
  // meas = gemessene Lage, target = gewünschte Lage (Grad, Vorzeichen wie
  // kin::BodyPose). active = false: Korrektur langsam abbauen.
  void update(float dt, bool active, float meas_roll, float meas_pitch, float target_roll,
              float target_pitch);

  float rollDeg() const { return roll_; }    // zur befohlenen Neigung addieren
  float pitchDeg() const { return pitch_; }
  bool fault() const { return fault_; }
  void clearFault() { fault_ = false; }

 private:
  void axis(float dt, float err, float& corr, float& sat_t);

  Config cfg_;
  float roll_ = 0.0f, pitch_ = 0.0f;
  float sat_roll_ = 0.0f, sat_pitch_ = 0.0f;  // Zeit in der Begrenzung
  bool fault_ = false;
};

}  // namespace leveling
