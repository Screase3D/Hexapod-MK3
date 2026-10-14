#pragma once
// "Lebendig wirken" – reine Logik ohne Arduino (Tests: test_host/life_test.cpp):
//
//   Stimmung   Steht er lange untätig herum, wird er gelangweilt (gähnt, seufzt, schaut
//              sich um) und setzt sich schließlich zum Schlafen hin. Eine Eingabe weckt ihn:
//              er steht auf und streckt sich.
//   Atmen      Im Stand hebt und senkt sich der Körper ganz leicht (die Augen atmen mit).
//   Zappeln    Wird er hochgehoben, zappelt er mit den Beinen – immer abwechselnd mit
//              einer Dreiergruppe, so steht er auch bei einer Fehlerkennung sicher.
//   Laufen     Trägheit (lehnt sich beim Anfahren zurück, schwingt beim Anhalten nach),
//              Blick in die Kurve bzw. Laufrichtung, Hüftschwung im Schritttakt.
//   Blick      Richtung für die Augen (-1 rechts … +1 links).
//
// Alles liefert Versätze wie "Lebendig" (alive::Output); die Bewegungssteuerung wendet sie
// nur so weit an, wie die Beine es erlauben.

#include <stdint.h>

#include "alive.h"

namespace life {

enum class Mood : uint8_t { Awake, Bored, Asleep };
const char* moodName(Mood m);  // "awake", "bored", "asleep"

struct Config {
  bool breathe = true;
  bool bored = true;
  bool lift = true;     // Zappeln beim Hochheben
  bool walk_fx = true;  // Laufen mit Charakter
  bool gaze = true;     // Augen schauen in die Bewegungsrichtung
  float sleep_s = 300.0f;  // so lange untätig, bis er sich schlafen legt (gelangweilt ab 40 %)
};

// ---- Stimmung
struct MoodIn {
  float dt = 0;
  bool enabled = true;
  bool standing = false;  // steht (IK, Haltung "stehen")
  bool still = false;     // steht still und nichts anderes läuft (darf eine Aktion spielen)
  bool activity = false;  // Eingabe in diesem Takt
  bool move = false;      // Eingabe will Bewegung (Laufen, Körper, Geste, Vorderbeine)
};
struct MoodOut {
  bool sit = false;    // hinsetzen (einschlafen)
  bool stand = false;  // aufstehen (aufwachen) – so lange gesetzt, bis er steht (Hinsetzen läuft evtl. noch)
  alive::Action play = alive::Action::None;
};

class Moods {
 public:
  explicit Moods(uint32_t seed = 0x9E3779B9u) : rng_(seed ? seed : 1u) {}
  MoodOut update(const MoodIn& in, float sleep_s);
  Mood mood() const { return mood_; }
  float idle() const { return idle_s_; }

 private:
  float rnd(float a, float b);
  Mood mood_ = Mood::Awake;
  float idle_s_ = 0.0f;
  float next_act_ = 0.0f;
  float sit_in_ = -1.0f;  // >= 0: nach dem letzten Gähnen hinsetzen
  bool stretch_ = false;  // nach dem Aufwachen strecken, sobald er steht
  bool wake_ = false;     // aufgeweckt: aufstehen, sobald möglich
  uint32_t rng_;
};

// ---- Atmen: Körperhöhe (mm) und Phase -1..1 für die Augen; blendet in ~1 s ein/aus
class Breath {
 public:
  float update(float dt, bool on);  // Versatz z
  float wave() const { return wave_ * level_; }  // -1..1 (mal Einblendung)
  // Abdunkeln der Augen im Atemtakt: 0 (eingeatmet bzw. aus) .. 0,22 (ausgeatmet)
  float eyeDim() const { return 0.22f * level_ * (0.5f - 0.5f * wave_); }

 private:
  float t_ = 0.0f;
  float level_ = 0.0f;
  float wave_ = 0.0f;
};

// ---- Hochheben erkennen (Lagesensor): plötzliche Beschleunigung bzw. Drehung, während
// er still steht. acc in g (Körper-Achsen), gyro in °/s.
class LiftDetector {
 public:
  bool update(float dt, bool armed, const float acc[3], const float gyro[3]);
  void reset() { score_ = 0.0f; armed_s_ = 0.0f; }

 private:
  float score_ = 0.0f;
  float armed_s_ = 0.0f;  // so lange schon scharf (kurz warten nach Bewegung)
};

// ---- Zappeln: Dreiergruppen abwechselnd hoch, Füße strampeln
class Flail {
 public:
  void start();
  void stop() { stop_ = true; }
  alive::Output update(float dt);
  bool active() const { return t_ >= 0.0f; }
  void reset() { t_ = -1.0f; level_ = 0.0f; }

 private:
  float t_ = -1.0f;  // < 0: aus
  float level_ = 0.0f;
  bool stop_ = false;
};

// ---- Laufen mit Charakter
struct WalkIn {
  float dt = 0;
  bool on = false;          // läuft bzw. läuft gerade aus
  float vx = 0, vy = 0;     // Ist-Geschwindigkeit (mm/s)
  float wz = 0;             // Drehrate (°/s, + links)
  float max_speed = 100;    // Höchstwerte zum Normieren
  float max_turn = 40;
  float phase = 0;          // Takt der Gangart 0..1
};
class WalkFx {
 public:
  gestures::Offset update(const WalkIn& in);
  bool stopped() const { return just_stopped_; }  // hat gerade angehalten (einmal)
  void reset();

 private:
  float last_v_[2] = {0, 0};
  float acc_f_[2] = {0, 0};   // gefilterte Beschleunigung
  float lean_[2] = {0, 0};    // Feder: Auslenkung ...
  float lean_v_[2] = {0, 0};  // ... und Geschwindigkeit
  float yaw_ = 0.0f;
  float pitch_ = 0.0f;
  float sway_ = 0.0f;         // Einblendung Hüftschwung
  float walk_s_ = 0.0f;       // so lange läuft er schon
  bool was_on_ = false;
  bool just_stopped_ = false;
  bool primed_ = false;
};

// Blickrichtung für die Augen aus Drehung/Seitwärtslaufen und Gieren des Körpers (°)
float gaze(float wz_norm, float vy_norm, float yaw_deg);

}  // namespace life
