#pragma once
// "Lebendig": Im Stand bewegt sich der Roboter ab und zu ganz leicht – schaut sich
// um, verlagert das Gewicht, legt den Kopf schief, hebt kurz ein Bein, tippt
// ungeduldig mit einem Vorderbein oder setzt die Füße einmal neu; dazu ein kaum
// sichtbares Atmen. Reine Mathematik (Tests: test_host/moves_test.cpp).
//
// Jede Aktion ist eine Folge von Stellungen (Körperversatz, höchstens ein Fuß
// angehoben), zwischen denen weich übergeblendet wird; am Ende ist alles wieder
// bei null. Zufall über einen eigenen Generator (Startwert wählbar, damit Tests
// reproduzierbar sind). Hebt immer nur ein Bein und verlagert vorher das Gewicht
// davon weg.

#include <stdint.h>

#include "gestures.h"
#include "kinematics.h"

namespace alive {

// Yawn/Sigh/Stretch wählt er nie zufällig – nur über play() (Langeweile, Aufwachen)
enum class Action : uint8_t { None, Look, Shift, Tilt, Lift, Tap, Shuffle, Yawn, Sigh, Stretch };
constexpr int NUM_ACTIONS = 10;
// "none", "look", "shift", "tilt", "lift", "tap", "shuffle", "yawn", "sigh", "stretch"
const char* actionName(Action a);

struct Output {
  gestures::Offset body;     // Körperversatz (zur Haltung addieren, wie Gesten)
  kin::Vec3 foot[6] = {};    // Fußversatz je Bein (Lauf-System), z = anheben
};

class Animator {
 public:
  // mounts: Montagerichtungen der Beine (für "Gewicht weg vom gehobenen Bein")
  explicit Animator(const kin::LegMount* mounts, uint32_t seed = 0x2545F491u);

  // enabled: Einstellung an. allowed: steht still und nichts anderes läuft.
  // Ohne Freigabe blendet er in ca. 0,4 s aus und wartet danach wieder eine Weile.
  Output update(float dt, bool enabled, bool allowed);

  Action action() const { return act_; }
  bool silent() const { return level_ <= 0.0f; }  // gibt nichts aus
  void start(Action a);  // nächste Aktion sofort (Tests)
  // Aktion abspielen, auch wenn "Lebendig" aus ist (sobald allowed); ersetzt eine laufende
  void play(Action a);
  bool playing() const { return forced_; }
  void setBreath(bool on) { breath_on_ = on; }  // eigenes leichtes Atmen (aus, wenn life atmet)
  void seed(uint32_t s) { rng_ = s ? s : 1u; }
  void reset();          // sofort still (z. B. IK aus)

 private:
  struct Key {
    gestures::Offset body;
    int8_t leg = -1;           // angehobenes Bein (-1 = keins)
    kin::Vec3 foot = {0, 0, 0};
    float move_s = 0.5f;       // Übergang hierher
    float hold_s = 0.0f;       // danach stehen bleiben
  };
  static constexpr int MAX_KEYS = 16;

  float rnd();                 // 0..1
  float rnd(float a, float b);
  void plan(Action a);
  Key& add(float move_s, float hold_s);
  gestures::Offset away(int leg, float mm) const;  // Körper weg vom Bein
  Output sample() const;       // aktueller Wert der Aktion

  const kin::LegMount* mounts_;
  uint32_t rng_;
  Action act_ = Action::None;
  Action last_ = Action::None;
  Key keys_[MAX_KEYS];
  int nkeys_ = 0;
  int key_ = 0;                // aktueller Übergang: von keys_[key_-1] nach keys_[key_]
  float key_t_ = 0.0f;
  float pause_ = 0.0f;         // Wartezeit bis zur nächsten Aktion
  float level_ = 0.0f;         // Ein-/Ausblenden 0..1
  float breath_t_ = 0.0f;
  bool forced_ = false;        // Aktion über play(): läuft auch ohne "Lebendig"
  bool breath_on_ = true;
};

}  // namespace alive
