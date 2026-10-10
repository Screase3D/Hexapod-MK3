#pragma once
// Bewegungssteuerung: verbindet Körperhaltung, inverse Kinematik und Servos.
//
// Modus "manual": Servos werden direkt gesetzt (Kalibrierung, Schieber).
// Modus "pose":   Füße stehen auf ihren Standardpunkten, die Körperhaltung
//                 (Höhe, Verschiebung, Rollen/Nicken/Gieren, Fußabstand) wird
//                 per IK in Gelenkwinkel umgerechnet. Phase 3 ergänzt hier die
//                 Gangarten.
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

// Aus dem Regel-Task: Haltung nachführen, IK rechnen, Servos setzen
void update(float dt_s);

void toJson(JsonObject obj);        // Telemetrie
void geometryToJson(JsonObject obj);  // Maße und Grenzen für die Oberfläche

}  // namespace motion
