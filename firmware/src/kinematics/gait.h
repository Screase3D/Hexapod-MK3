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
//
// Takt: Eine gemeinsame Phase läuft durch den Zyklus; jedes Bein hat einen Versatz
// dazu (Beinphase = Takt - Versatz, 0 = Beginn seines Schwungs). Im eingeschwungenen
// Lauf sind die Versätze die des Musters der Gangart.
// Gangwechsel im Lauf: zu einer langsameren Gangart erst abbremsen, dann umtakten;
// zu einer schnelleren erst umtakten, dann beschleunigen. Beim Umtakten bekommt
// jedes Bein die Phase, die zu seiner Fußstellung passt (ein laufender Schwung wird
// in seinem Tempo zu Ende geführt); danach finden die Beine in den Standphasen ins
// neue Muster (Standphase bis zu 10 % länger bzw. 60 % kürzer, Tempo solange 75 %).
// Ein Bein hebt nie ab, solange ein Nachbarbein in der Luft ist.
// Würde ein Standfuß vor seinem Schwung den Rand der Reichweite erreichen
// (Richtungsumkehr, Anlaufen in Kurven), setzt er vorher um und findet danach wieder
// ins Muster. Die Schwungbahn folgt einem sich verschiebenden Aufsetzpunkt ohne
// Sprung und weicht nah am Körper nach außen aus (Knie, setSwingGuard).
// Springen die Neutralpunkte im Stillstand um mehr als 10 mm (Aufstehen),
// werden die Füße ebenso mit Schritten umgesetzt.
//
// Tempo: Ein Fuß legt je Standphase höchstens max_stride zurück. Zusätzlich
// (setReach) darf er der Coxa-Achse nicht näher kommen als min_radius: Ganz
// innen müsste das Knie zum Anheben sonst zu weit einknicken. Die Grenze wirkt
// richtungsabhängig – Schritte quer zum Bein (Mittelbeine beim Vorwärtslaufen)
// dürfen lang sein, Schritte zum Körper hin nur so lang wie nötig.

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
  float ramp_s = 0.5f;        // Zeit von 0 auf volle Geschwindigkeit (beim Anlaufen ggf. länger)
};

// Anteil der Standphase am Zyklus (Rest = Schwungphase)
float dutyFactor(Type t);
// Dauer eines vollen Zyklus (alle Beine je einmal geschwungen)
float cycleTime(Type t, float swing_s);

class Gait {
 public:
  // Alle Füße auf die Neutralpunkte, Stillstand
  void reset(const kin::Vec3 neutral[NUM_LEGS]);

  // Gangart-Wechsel wirkt auch im Lauf (fließender Übergang, siehe oben)
  void setParams(const Params& p);
  const Params& params() const { return params_; }

  // Fahrbefehl, normiert auf -1..1: vor/zurück, links/rechts, Drehen (+ = links)
  void setCommand(float vx, float vy, float turn);

  // Reichweite: coxa = Coxa-Achsen im Lauf-System (x, y); min/max_radius =
  // erlaubter waagerechter Abstand Fuß <-> Coxa-Achse (siehe minFootRadius,
  // maxFootRadius). Ohne Aufruf gilt nur max_stride.
  void setReach(const kin::Vec3 coxa[NUM_LEGS], float min_radius, float max_radius);
  // Schwungbahn-Schutz: Hebt ein Fuß nah am Körper ab und schwingt nicht gleich nach
  // außen (Richtungswechsel, Zwischenschritt), führt die Bahn ihn so weit nach außen,
  // dass das Knie (bei Körperhöhe height) nicht unter knee_min_deg einknickt.
  void setSwingGuard(const kin::LegGeometry& g, float height, float knee_min_deg);

  // Einen Regelschritt rechnen. neutral = aktuelle Neutralpunkte (Fußabstand
  // kann sich ändern), im Lauf-System auf dem Boden.
  void update(float dt, const kin::Vec3 neutral[NUM_LEGS]);

  const kin::Vec3& foot(int leg) const { return legs_[leg].pos; }
  bool swinging(int leg) const { return legs_[leg].swing; }
  uint8_t swingMask() const;
  bool idle() const { return idle_; }
  float phase() const { return phase_; }
  Type activeType() const { return active_type_; }
  // Gangwechsel läuft noch (Tempo angepasst, Beine finden ins neue Muster)
  bool transitioning() const { return !idle_ && (switching_ || params_.type != active_type_); }
  float phaseError() const { return phase_err_; }  // größte Abweichung vom Muster (Anteil Zyklus)

  // Tatsächliche (gerampte, begrenzte) Geschwindigkeit
  float speedX() const { return vx_; }      // mm/s
  float speedY() const { return vy_; }      // mm/s
  float turnRate() const { return wz_; }    // rad/s
  // Höchstwerte bei den aktuellen Parametern (für die Anzeige)
  float maxSpeed() const { return max_speed_; }  // mm/s geradeaus (mit Reichweitengrenze)
  float maxSide() const { return max_side_; }    // mm/s seitwärts (mit Reichweitengrenze)
  float maxTurn() const { return max_turn_; }    // rad/s
  float nominalSpeed() const { return nominal_speed_; }  // mm/s ohne Reichweitengrenze
  // aktuelle Schrittfrequenz: 1 = eingestellte Schrittdauer, 2 = doppelt so lang
  float cadence() const { return cadence_; }

 private:
  struct Leg {
    kin::Vec3 pos{0, 0, 0};
    bool swing = false;  // Fuß in der Luft
    float off = 0.0f;    // Versatz gegen den Takt (Muster: Schwungbeginn der Gangart)
    float ph = 0.0f;     // Beinphase im letzten Schritt 0..1 (0 = Beginn des Schwungs)
    float u = 0.0f;      // Fortschritt des laufenden Schwungs 0..1
    float h = 0.0f;      // davon zurückgelegter Weg 0..1 (Profil)
    float dur = 0.0f;    // > 0: Schwung nach Zeit mit dieser Dauer (begann vor dem Umtakten)
    float half = 0.0f;   // dabei: Vorlauf des Aufsetzpunkts (halbe Standphase der alten Gangart, s)
    bool wait = false;   // Schwung fällig, aber noch nicht erlaubt (Nachbar in der Luft)
  };

  float footMax(Type t) const;  // Fußgeschwindigkeit bei max_stride je Standphase
  bool liftAllowed(int leg) const;
  // Schrittfrequenz für die neue Gangart, wenn umgetaktet werden kann (sonst 0)
  float switchCadence(float dt, const kin::Vec3 neutral[NUM_LEGS]) const;
  void switchType(float cadence, const kin::Vec3 neutral[NUM_LEGS]);
  // Beinphase passend zur Fußstellung (Standbein): Schwungbeginn, wenn der Fuß das
  // hintere Ende der Standphase erreicht. done = Anteil der Standphase, falls fast im Stand.
  float footPhase(int leg, float done, const kin::Vec3 neutral[NUM_LEGS]) const;
  // Zeit (s), bis der Standfuß den Rand der Reichweite erreicht (1e9: entfernt sich / keine Grenze)
  float reachTime(int leg) const;

  void computeVelocity(float dt, const kin::Vec3 neutral[NUM_LEGS]);
  // größter Faktor 0..1 für (vx, vy, wz), mit dem alle Füße in der Reichweite bleiben
  float reachScale(float vx, float vy, float wz, float half_stance_s, const kin::Vec3 neutral[NUM_LEGS]) const;

  Params params_;
  Type active_type_ = Type::Tripod;
  Type from_type_ = Type::Tripod;  // Gangart vor dem letzten Umtakten
  Leg legs_[NUM_LEGS];
  float cmd_[3] = {0, 0, 0};       // Vorgabe (normiert)
  float cmd_ramped_[3] = {0, 0, 0};
  float vx_ = 0, vy_ = 0, wz_ = 0;
  float max_speed_ = 0, max_side_ = 0, max_turn_ = 0, nominal_speed_ = 0;
  float cadence_ = 1.0f;
  float phase_ = 0;           // Takt 0..1
  float vmax_ = 0;            // Grenze der Fußgeschwindigkeit (gleitet beim Gangwechsel)
  bool converging_ = false;   // Beine finden ins Muster (nach dem Umtakten oder einem Zwischenschritt)
  bool switching_ = false;    // Gangwechsel bis zum Abschluss (Tempo reduziert)
  float phase_err_ = 0;
  bool idle_ = true;
  bool starting_ = true;  // erster Zyklus nach dem Stillstand: langsamer beschleunigen
  kin::Vec3 coxa_[NUM_LEGS] = {};
  float min_radius_ = -1.0f;  // < 0: keine Reichweitengrenze
  float max_radius_ = 0.0f;
  float reach_k_ = 1.0f;      // aktueller Faktor aus der Reichweitengrenze
  float guard_coxa_ = 0.0f, guard_h_ = 0.0f, guard_d2_ = -1.0f;  // Schwungbahn-Schutz (d2 < 0: aus)
};

// Hilfsprofile (auch für Tests)
float smoothStep5(float u);  // 0..1, Steigung und Krümmung an den Enden 0
float liftProfile(float u);  // 0..1..0, Steigung an den Enden 0

// Kleinster Abstand Fuß <-> Coxa-Achse (waagerecht), bei dem ein Schritt mit
// Fußhub step_height bei Körperhöhe height den Kniewinkel (zwischen Femur-Linie
// und Linie Knie -> Fußspitze) nicht unter knee_min_deg drückt. Gerechnet für
// einen Schritt genau zum Körper hin bzw. weg (ungünstigster Fall) um den
// Neutralpunkt im Abstand stance. Ergebnis >= stance - (größte Auslenkung).
float minFootRadius(const kin::LegGeometry& g, float stance, float height, float step_height,
                    float knee_min_deg);
// Größter Abstand Fuß <-> Coxa-Achse (waagerecht) am Boden, mit 3 % Reserve
// vor dem gestreckten Bein.
float maxFootRadius(const kin::LegGeometry& g, float height);

}  // namespace gait
