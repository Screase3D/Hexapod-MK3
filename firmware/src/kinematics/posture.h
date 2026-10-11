#pragma once
// Aufstehen / Hinsetzen – reine Logik ohne Arduino-Abhängigkeiten (Tests: test_host/).
//
// Sitzen:      Körper liegt auf, Füße weit außen auf dem Boden (Sitzhaltung).
// Aufstehen:   1. Körper bei weit stehenden Füßen ruckfrei auf Standhöhe heben
//                 (Füße bleiben stehen),
//              2. Füße mit Schritten auf den normalen Fußabstand setzen
//                 (die Gangart macht das, sobald sich die Neutralpunkte ändern).
// Hinsetzen:   umgekehrt – erst Füße nach außen setzen und Körper gerade
//              ausrichten, dann absenken.
//
// Der Sequenzer liefert nur Höhe und Fußabstand; die Bewegungssteuerung
// übernimmt sie direkt (statt der Regler) und richtet den Körper dabei gerade aus.

#include <stdint.h>

namespace posture {

enum class State : uint8_t { Sitting, StandingUp, Standing, SittingDown };
const char* stateName(State s);  // "sitting", "standing_up", "standing", "sitting_down"

struct Config {
  float sit_height;  // Höhe der Coxa-Achsen über dem Boden im Sitzen (mm)
  float sit_stance;  // Fußabstand im Sitzen (mm, von der Coxa-Achse)
  float rise_s;      // Dauer des Anhebens bzw. Absenkens
};

struct Output {
  bool active;   // true = Höhe/Fußabstand vorgegeben, Körper gerade ausrichten
  float height;
  float stance;
};

class Sequencer {
 public:
  explicit Sequencer(const Config& c) : cfg_(c) {}

  void reset(State s);
  State state() const { return state_; }
  bool busy() const { return state_ == State::StandingUp || state_ == State::SittingDown; }

  // false, wenn gerade nicht möglich (z. B. mitten im Hinsetzen)
  bool requestStand();
  bool requestSit();

  // stand_height/stand_stance: Zielwerte im Stehen (Regler der Körperhaltung),
  // current_height: aktuelle Körperhöhe, gait_idle: Gang steht still
  Output update(float dt, float stand_height, float stand_stance, float current_height,
                bool gait_idle);

 private:
  Config cfg_;
  State state_ = State::Sitting;
  int phase_ = 0;
  float t_ = 0;
  float h0_ = 0;  // Höhe zu Beginn des Absenkens
};

}  // namespace posture
