#pragma once
// Vorderbeine als "Arme" – reine Mathematik (Tests: test_host/moves_test.cpp).
//
// Ablauf (statisch stabil, der Schwerpunkt bleibt immer über der Standfläche):
//   1. Gewicht verlagern: Körper nach hinten, Mittelbeine einen Schritt nach vorne
//      (die Gangart setzt sie mit Schritten um).
//   2. Vorderbeine heben: Die Vorderfüße lösen sich von der Gangart und schwingen
//      in die Bereitstellung vor dem Körper.
//   3. Steuern: Jeder Vorderfuß folgt seiner Vorgabe (vor/zurück, hoch/runter),
//      "Winken" lässt den rechten winken.
//   4. Absenken und Gewicht zurück – umgekehrt.
// Laufen ist in dieser Zeit gesperrt (der Aufrufer prüft active()).

#include <stdint.h>

#include "kinematics.h"

namespace arms {

enum class State : uint8_t { Down, ShiftBack, Raise, Up, Lower, ShiftForward };
const char* stateName(State s);  // "down", "shift_back", "raise", "up", "lower", "shift_forward"

struct Config {
  float cog_shift = 35.0f;         // Körper nach hinten (mm)
  float mid_forward = 25.0f;       // Mittelbeine nach vorne (mm)
  float shift_s = 1.0f;            // Dauer der Gewichtsverlagerung
  float raise_s = 1.0f;            // Dauer Heben bzw. Absenken
  // Bereitstellung relativ zur Neutralstellung (Lauf-System). Weil der Körper
  // nach hinten verlagert ist, steht der Fuß damit schon 35 mm weiter vor der Coxa.
  kin::Vec3 ready = {0, 0, 50};
  float range_fwd = 30.0f;         // Steuerbereich nach vorne um die Bereitstellung
  float range_back = 15.0f;        // ... nach hinten (weiter zurück + hoch: Femur am Anschlag)
  float range_z = 30.0f;           // Steuerbereich hoch/runter (20..80 mm über dem Boden)
  float follow_mm_s = 150.0f;      // so schnell folgen die Füße der Steuerung
  float wave_s = 3.0f;             // Dauer des Winkens
};

struct Output {
  float body_dx = 0.0f;            // zur Körperhaltung x addieren
  float mid_dx = 0.0f;             // Neutralpunkte der Mittelbeine nach vorne
  float blend = 0.0f;              // 0..1: Vorderfüße folgen foot[] statt der Gangart
  kin::Vec3 foot[2] = {};          // Versatz Vorderfuß links (Bein 1) / rechts (Bein 4)
};

class Controller {
 public:
  explicit Controller(const Config& cfg = Config()) : cfg_(cfg) {}

  void reset();         // sofort "unten" (z. B. Torque aus, Not-Aus)
  bool requestUp();     // nur aus "unten"
  bool requestDown();   // aus jedem anderen Zustand
  // Vorgabe je Vorderfuß, -1..1: x = vor(+)/zurück, z = hoch(+)/runter
  void setControl(float lx, float lz, float rx, float rz);
  bool wave();          // nur "oben": rechtes Vorderbein winkt

  Output update(float dt, bool gait_idle);

  State state() const { return state_; }
  bool active() const { return state_ != State::Down; }
  bool waving() const { return wave_t_ >= 0.0f; }

 private:
  Config cfg_;
  State state_ = State::Down;
  float t_ = 0.0f;
  float shift_from_ = 0.0f;        // body_dx zu Beginn einer Verlagerung
  kin::Vec3 from_[2] = {};         // Fußversatz zu Beginn von Heben/Absenken
  kin::Vec3 cur_[2] = {};          // aktueller Fußversatz
  float ctrl_[4] = {0, 0, 0, 0};   // lx, lz, rx, rz
  float wave_t_ = -1.0f;           // < 0: kein Winken
  Output out_;
};

}  // namespace arms
