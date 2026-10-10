#pragma once
// Mechanische Maße des Hexapods für die inverse Kinematik.
//
// !!! PLATZHALTER – noch nicht am echten Roboter gemessen !!!
// Bitte nachmessen und hier eintragen, dann GEOMETRY_IS_PLACEHOLDER = false:
//   coxa   Abstand Coxa-Drehachse -> Femur-Drehachse (waagerecht)
//   femur  Abstand Femur-Drehachse -> Knie-Drehachse
//   tibia  Abstand Knie-Drehachse -> Fußspitze
//   Montage: Position jeder Coxa-Drehachse relativ zur Körpermitte
//            (x nach vorne, y nach links, in mm) und die Richtung, in die
//            das Bein in Neutralstellung (Coxa 0°) zeigt (Grad, gegen den
//            Uhrzeigersinn ab "vorne"; links positiv, rechts negativ).
// Alle Längen in mm.

#include "kinematics/kinematics.h"

constexpr bool GEOMETRY_IS_PLACEHOLDER = true;

constexpr kin::LegGeometry LEG_GEOMETRY = {
    30.0f,  // coxa
    60.0f,  // femur
    90.0f,  // tibia
};

// Reihenfolge wie SERVO_MAP: Bein 1-3 links (vorne, Mitte, hinten), 4-6 rechts
constexpr kin::LegMount LEG_MOUNTS[6] = {
    {60.0f, 40.0f, 45.0f},      // Bein 1 vorne links
    {0.0f, 50.0f, 90.0f},       // Bein 2 Mitte links
    {-60.0f, 40.0f, 135.0f},    // Bein 3 hinten links
    {60.0f, -40.0f, -45.0f},    // Bein 4 vorne rechts
    {0.0f, -50.0f, -90.0f},     // Bein 5 Mitte rechts
    {-60.0f, -40.0f, -135.0f},  // Bein 6 hinten rechts
};

// Körperhaltung: Standardwerte und Grenzen der Oberfläche
struct PoseLimits {
  float height_min, height_max, height_default;  // Körperhöhe über Boden
  float stance_min, stance_max, stance_default;  // Fußabstand von der Coxa-Achse
  float shift_max;                               // Verschieben x/y
  float tilt_max;                                // Rollen/Nicken
  float yaw_max;                                 // Gieren
};

constexpr PoseLimits POSE_LIMITS = {
    20.0f, LEG_GEOMETRY.tibia + 0.5f * LEG_GEOMETRY.femur, 0.8f * LEG_GEOMETRY.tibia,
    LEG_GEOMETRY.coxa + 0.4f * LEG_GEOMETRY.femur,
    LEG_GEOMETRY.coxa + LEG_GEOMETRY.femur + 0.6f * LEG_GEOMETRY.tibia,
    LEG_GEOMETRY.coxa + LEG_GEOMETRY.femur,
    40.0f,
    20.0f,
    25.0f,
};

// Geschwindigkeit, mit der die Körperhaltung den Vorgaben folgt
constexpr float POSE_SPEED_MM_PER_S = 80.0f;
constexpr float POSE_SPEED_DEG_PER_S = 45.0f;
