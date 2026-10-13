#pragma once
// Vorderbeine als "Arme" – reine Mathematik (Tests: test_host/moves_test.cpp).
//
// Ablauf (statisch stabil, der Schwerpunkt bleibt immer über der Standfläche):
//   1. Gewicht verlagern: Körper nach hinten, Mittelbeine einen Schritt nach vorne
//      (die Gangart setzt sie mit Schritten um).
//   2. Vorderbeine heben: Die Vorderfüße lösen sich von der Gangart und gehen
//      senkrecht nach oben in die Bereitstellung.
//   3. Steuern: Jeder Vorderfuß folgt seiner Vorgabe – schwenken (um die Coxa),
//      hoch/runter und weiter weg/heran. Der Arbeitsraum ist nicht fest: Jeder
//      Zielpunkt wird über den Rückruf `reachable` geprüft (IK, Gelenkgrenzen),
//      der Fuß geht so weit, wie das Bein reicht. "Winken" lässt den rechten winken.
//   4. Zurück in die Bereitstellung, absetzen und Gewicht zurück – umgekehrt.
// Laufen ist in dieser Zeit gesperrt (der Aufrufer prüft active()).
//
// Alle Fußpunkte im Lauf-System (wie die Gangart): z = Höhe über dem Boden.

#include <stdint.h>

#include "kinematics.h"

namespace arms {

enum class State : uint8_t { Down, ShiftBack, Raise, Up, Lower, ShiftForward };
const char* stateName(State s);  // "down", "shift_back", "raise", "up", "lower", "shift_forward"

// Kann Vorderbein k (0 = links/Bein 1, 1 = rechts/Bein 4) den Fußpunkt erreichen?
using Reachable = bool (*)(void* ctx, int k, const kin::Vec3& foot);

// Lage der Vorderbeine in diesem Zyklus
struct Frame {
  kin::Vec3 coxa[2];               // Coxa-Achsen (Lauf-System, z egal)
  kin::Vec3 ground[2];             // Neutralpunkte der Vorderfüße auf dem Boden
  Reachable reachable = nullptr;   // nullptr = alles erreichbar
  void* ctx = nullptr;
};

struct Config {
  float cog_shift = 35.0f;         // Körper nach hinten (mm)
  float mid_forward = 25.0f;       // Mittelbeine nach vorne (mm)
  float shift_s = 1.0f;            // Dauer der Gewichtsverlagerung
  float raise_s = 1.0f;            // Dauer Heben bzw. Absetzen
  float ready_h = 60.0f;           // Bereitstellung: senkrecht über dem Neutralpunkt, so hoch
  float sweep_deg = 32.0f;         // Schwenken je Richtung (innen = nach vorne, außen = zur Seite)
  float up = 100.0f;               // Höhe: bis ready_h + up ...
  float down = 45.0f;              // ... bzw. ready_h - down über dem Boden
  float reach_out = 35.0f;         // Abstand zur Coxa: weiter weg ...
  float reach_in = 40.0f;          // ... bzw. heran (um den Abstand der Bereitstellung)
  float speed_mm_s = 220.0f;       // Höchstgeschwindigkeit der Füße
  float accel_mm_s2 = 1200.0f;     // Beschleunigung (weiches Anfahren und Bremsen)
  float wave_s = 3.0f;             // Dauer des Winkens
};

struct Output {
  float body_dx = 0.0f;            // zur Körperhaltung x addieren
  float mid_dx = 0.0f;             // Neutralpunkte der Mittelbeine nach vorne
  float blend = 0.0f;              // 0..1: Vorderfüße folgen foot[] statt der Gangart
  kin::Vec3 foot[2] = {};          // Vorderfuß links (Bein 1) / rechts (Bein 4), Lauf-System
};

class Controller {
 public:
  explicit Controller(const Config& cfg = Config()) : cfg_(cfg) {}

  void reset();         // sofort "unten" (z. B. Torque aus, Not-Aus)
  bool requestUp();     // nur aus "unten"
  bool requestDown();   // aus jedem anderen Zustand
  // Vorgabe je Vorderfuß, -1..1: x = schwenken (+ = nach innen/vorne),
  // z = hoch (+)/runter; reach = beide weiter weg (+)/heran
  void setControl(float lx, float lz, float rx, float rz, float reach = 0.0f);
  bool wave();          // nur "oben": rechtes Vorderbein winkt

  Output update(float dt, bool gait_idle, const Frame& frame);

  State state() const { return state_; }
  bool active() const { return state_ != State::Down; }
  bool waving() const { return wave_t_ >= 0.0f; }
  bool limited() const { return limited_; }  // Vorgabe reicht weiter als das Bein

 private:
  // Fußlage um die Coxa: Richtung (rad, Lauf-System), waagerechter Abstand, Höhe
  struct Polar {
    float phi = 0, r = 0, h = 0;
  };
  struct Axis {  // weiche Folgebewegung (Geschwindigkeit und Beschleunigung begrenzt)
    float v = 0;
  };

  Polar groundPolar(const Frame& f, int k) const;
  kin::Vec3 toFoot(const Frame& f, int k, const Polar& p) const;
  bool reach(const Frame& f, int k, const Polar& p) const;
  bool follow(const Frame& f, int k, const Polar& target, float dt);  // true = angekommen
  float homeHeight(const Frame& f, int k) const;
  void startDescent();

  Config cfg_;
  State state_ = State::Down;
  float t_ = 0.0f;
  float shift_from_ = 0.0f;        // body_dx zu Beginn einer Verlagerung
  Polar cur_[2];                   // aktuelle Fußlage
  Axis ax_[2][2];                  // Folgebewegung je Fuß: Schwenken, Höhe
  Polar lower_from_[2];            // Fußlage zu Beginn des Absetzens
  bool descending_ = false;        // Absetzen: zurück in der Bereitstellung, jetzt senkrecht runter
  float ctrl_[5] = {0, 0, 0, 0, 0};  // lx, lz, rx, rz, reach
  float wave_t_ = -1.0f;           // < 0: kein Winken
  bool limited_ = false;
  Output out_;
};

}  // namespace arms
