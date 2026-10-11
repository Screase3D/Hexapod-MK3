#pragma once
// Gangarten – reine Mathematik ohne Arduino-Abhängigkeiten (Tests: test_host/).
//
// Die Füße werden im Lauf-System geführt: Ursprung auf dem Boden unter der
// Körpermitte, x vorne, y links, z oben – es bewegt und dreht sich mit dem
// Roboter. Die Körperhaltung (Höhe, Neigung, ...) kommt danach obendrauf.
//
// Ablauf je Bein (phasengesteuert):
//   Standphase:  Fuß steht auf dem Boden; im Lauf-System wandert er genau
//                entgegen der Körperbewegung (v, Drehrate) -> rutscht nicht.
//   Schwungphase: Fuß hebt ab und schwingt zum Aufsetzpunkt vor der
//                Neutralstellung (Profile ohne Ruck: Geschwindigkeit an Anfang
//                und Ende 0, kein Aufschlagen).
// Ohne Fahrbefehl laufen die Beine noch in die Neutralstellung zurück und
// bleiben dann stehen; Beine, die schon dort stehen, heben nicht ab.
// Springen die Neutralpunkte im Stillstand um mehr als 10 mm (Aufstehen),
// werden die Füße ebenso mit Schritten umgesetzt.

#include <stdint.h>

#include "kinematics.h"

namespace gait {

constexpr int NUM_LEGS = 6;  // Reihenfolge wie LEG_MOUNTS: 0-2 links v/m/h, 3-5 rechts

enum class Type : uint8_t {
  Tripod,  // je 3 Beine gleichzeitig (schnell)
  Ripple,  // je 2 Beine (ein Bein pro Seite), ruhiger
  Wave,    // immer nur 1 Bein (langsam, sehr stabil)
};
constexpr int NUM_TYPES = 3;
const char* typeName(Type t);  // "tripod", "ripple", "wave"
bool typeFromName(const char* name, Type& out);

struct Params {
  Type type = Type::Tripod;
  float swing_s = 0.5f;       // Dauer eines Fußschwungs (Schritt); Zyklus = swing_s / (1 - duty)
  float step_height = 20.0f;  // Fußhub in mm
  float max_stride = 40.0f;   // größter Fußweg während einer Standphase in mm
  float ramp_s = 0.5f;        // Zeit von 0 auf volle Geschwindigkeit
};

// Anteil der Standphase am Zyklus (Rest = Schwungphase)
float dutyFactor(Type t);
// Dauer eines vollen Zyklus (alle Beine je einmal geschwungen)
float cycleTime(Type t, float swing_s);

class Gait {
 public:
  // Alle Füße auf die Neutralpunkte, Stillstand
  void reset(const kin::Vec3 neutral[NUM_LEGS]);

  // Gangart-Wechsel wird erst im Stillstand wirksam (siehe pendingType)
  void setParams(const Params& p);
  const Params& params() const { return params_; }

  // Fahrbefehl, normiert auf -1..1: vor/zurück, links/rechts, Drehen (+ = links)
  void setCommand(float vx, float vy, float turn);

  // Einen Regelschritt rechnen. neutral = aktuelle Neutralpunkte (Fußabstand
  // kann sich ändern), im Lauf-System auf dem Boden.
  void update(float dt, const kin::Vec3 neutral[NUM_LEGS]);

  const kin::Vec3& foot(int leg) const { return legs_[leg].pos; }
  bool swinging(int leg) const { return legs_[leg].swing; }
  uint8_t swingMask() const;
  bool idle() const { return idle_; }
  float phase() const { return phase_; }
  Type activeType() const { return active_type_; }

  // Tatsächliche (gerampte, begrenzte) Geschwindigkeit
  float speedX() const { return vx_; }      // mm/s
  float speedY() const { return vy_; }      // mm/s
  float turnRate() const { return wz_; }    // rad/s
  // Höchstwerte bei den aktuellen Parametern (für die Anzeige)
  float maxSpeed() const { return max_speed_; }  // mm/s
  float maxTurn() const { return max_turn_; }    // rad/s

 private:
  struct Leg {
    kin::Vec3 pos{0, 0, 0};
    kin::Vec3 lift{0, 0, 0};  // Abhebepunkt der laufenden Schwungphase
    bool swing = false;       // Fuß in der Luft
    bool in_window = false;   // Phase liegt im Schwungfenster (auch wenn übersprungen)
  };

  void computeVelocity(float dt, const kin::Vec3 neutral[NUM_LEGS]);

  Params params_;
  Type active_type_ = Type::Tripod;
  Leg legs_[NUM_LEGS];
  float cmd_[3] = {0, 0, 0};       // Vorgabe (normiert)
  float cmd_ramped_[3] = {0, 0, 0};
  float vx_ = 0, vy_ = 0, wz_ = 0;
  float max_speed_ = 0, max_turn_ = 0;
  float phase_ = 0;
  bool idle_ = true;
};

// Hilfsprofile (auch für Tests)
float smoothStep5(float u);  // 0..1, Steigung und Krümmung an den Enden 0
float liftProfile(float u);  // 0..1..0, Steigung an den Enden 0

}  // namespace gait
