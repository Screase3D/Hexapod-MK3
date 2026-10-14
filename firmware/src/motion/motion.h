#pragma once
// Bewegungssteuerung: verbindet Körperhaltung, Gangart, inverse Kinematik und Servos.
//
// Modus "manual": Servos werden direkt gesetzt (Kalibrierung, Schieber).
// Modus "pose":   IK-Betrieb. Die Gangart führt die Füße (im Stand auf ihren
//                 Neutralpunkten, beim Laufen in Schritten), die Körperhaltung
//                 (Höhe, Verschiebung, Rollen/Nicken/Gieren, Fußabstand) kommt
//                 obendrauf; die IK rechnet daraus die Gelenkwinkel.
//                 Haltung: sitzen -> aufstehen -> stehen/laufen -> hinsetzen.
//
// update() läuft im Regel-Task; alle anderen Funktionen sind thread-sicher.

#include <Arduino.h>
#include <ArduinoJson.h>

namespace motion {

enum class Mode : uint8_t { Manual, Pose };

void begin();
void setMode(Mode m);
Mode mode();

// Übernimmt die im JSON vorhandenen Felder als neue Zielhaltung:
// x, y, z (Höhe), roll, pitch, yaw, stance. Werte werden auf die Reglergrenzen
// begrenzt; erreichen die Beine die Haltung nicht (oder stoßen an Gelenkgrenzen
// der Kalibrierung), wird sie auf den nächsten zulässigen Wert zurückgenommen:
// Rückgabe true, note erklärt was und warum.
bool applyPoseJson(JsonVariantConst obj, String& note);
void resetPose();

// Aufstehen / Hinsetzen (schalten in den IK-Modus). Ohne Torque oder bei
// Not-Aus gilt der Roboter als liegend (Sitzhaltung); Laufen geht nur im Stehen.
void stand();
void sit();

// Fahrbefehl, normiert -1..1 (vx vor, vy links, turn + = links drehen).
// Schaltet in den IK-Modus. Bleibt ein neuer Befehl länger als
// WALK_TIMEOUT_MS aus (Verbindung weg), hält der Roboter an.
void walk(float vx, float vy, float turn);
void stopWalking();
// Gangparameter: type ("tripod"/"ripple"/"wave"), swing_s, step_height
bool applyGaitJson(JsonVariantConst obj, String& err);
const char* cycleGait(int dir);  // nächste (+1) / vorige (-1) Gangart, liefert den Namen
// Höhe um dz ändern (Controller); true = begrenzt, note erklärt warum
bool nudgeHeight(float dz, String& note);

// Körper-Joystick: Versatz normiert -1..1 (x vor, y links, roll, pitch, yaw), wird
// zur Haltung addiert (nur im Stehen). Ohne neuen Befehl für WALK_TIMEOUT_MS
// federt der Körper zurück. Nicht erreichbare Anteile werden zurückgenommen.
void body(float x, float y, float roll, float pitch, float yaw);

// Vorderbeine: heben (Gewicht nach hinten, Mittelbeine vor, Vorderbeine hoch) bzw.
// absetzen. Heben nur im Stehen und Stillstand; solange sie oben sind, wird
// nicht gelaufen. armControl: je Vorderfuß schwenken (+ = nach innen/vorne) und
// hoch(+)/runter, reach = beide weiter weg (+)/heran; normiert -1..1, federt ohne
// neue Befehle in die Bereitstellung zurück. Der Fuß geht so weit, wie das Bein reicht.
bool setArms(bool up, String& err);
void armControl(float lx, float lz, float rx, float rz, float reach = 0.0f);
bool wave(String& err);  // rechtes Vorderbein winkt (nur wenn oben)

// Gesten im Stand: "circle", "nod", "shake", "pushup", "dance"; "stop" beendet
bool gesture(const char* name, String& err);

// "Lebendig": im Stand ab und zu kleine zufällige Bewegungen (umschauen, Gewicht
// verlagern, ein Bein kurz heben, ...). Jederzeit an/aus, wird gespeichert.
void setAlive(bool on);

// Lageausgleich: hält den Körper mit dem Lagesensor waagerecht (bzw. auf der
// eingestellten Neigung). Schaltet sich bei falscher Sensorrichtung selbst ab.
void setLeveling(bool on);

// Haltung, Gang, Lageausgleich, Lebendig und Stellgeschwindigkeit werden gespeichert und
// beim Start wiederhergestellt. notifySettingsChanged() z. B. nach "slew";
// loop() (aus loop(), nicht dem Regel-Task) schreibt verzögert ins NVS.
void notifySettingsChanged();
void loop();

// Aus dem Regel-Task: Haltung nachführen, Gang rechnen, IK, Servos setzen
void update(float dt_s);

// Was gerade passiert (für LEDs und Controller)
struct Activity {
  bool pose = false;      // IK-Modus
  bool standing = false;  // steht (Übergang abgeschlossen)
  bool sitting = false;
  bool walking = false;
  bool arms = false;      // Vorderbeine nicht unten
  bool arms_up = false;   // Vorderbeine oben (steuerbar)
  bool gesture = false;
  bool alive = false;     // Einstellung "Lebendig"
  bool leveling = false;  // Einstellung Lageausgleich
  bool level_fault = false;  // Lageausgleich wegen falscher Sensorrichtung abgeschaltet
};
Activity activity();

void toJson(JsonObject obj);          // Telemetrie
// Live-Ansicht (ca. 20 Hz): "ik" (IK-Betrieb), "b" Körperlage der IK [x, y, z, Rollen,
// Nicken, Gieren], "s" Beine in der Luft (Bitmaske), "v" Tempo [vx, vy mm/s, Drehen °/s]
void liveToJson(JsonObject obj);
void geometryToJson(JsonObject obj);  // Maße und Grenzen für die Oberfläche

}  // namespace motion
