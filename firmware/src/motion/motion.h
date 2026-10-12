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
// x, y, z (Höhe), roll, pitch, yaw, stance. Werte werden begrenzt.
void applyPoseJson(JsonVariantConst obj);
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

// Lageausgleich: hält den Körper mit dem Lagesensor waagerecht (bzw. auf der
// eingestellten Neigung). Schaltet sich bei falscher Sensorrichtung selbst ab.
void setLeveling(bool on);

// Haltung, Gang, Lageausgleich und Stellgeschwindigkeit werden gespeichert und
// beim Start wiederhergestellt. notifySettingsChanged() z. B. nach "slew";
// loop() (aus loop(), nicht dem Regel-Task) schreibt verzögert ins NVS.
void notifySettingsChanged();
void loop();

// Aus dem Regel-Task: Haltung nachführen, Gang rechnen, IK, Servos setzen
void update(float dt_s);

void toJson(JsonObject obj);          // Telemetrie
void geometryToJson(JsonObject obj);  // Maße und Grenzen für die Oberfläche

}  // namespace motion
