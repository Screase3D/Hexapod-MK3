#pragma once
// Mechanische Maße des Hexapods für die inverse Kinematik.
//
// Maße aus dem CAD-Modell (Zeichnung: docs/beingeometrie.svg), alle Längen in mm
// zwischen den Drehachsen:
//   coxa   Abstand Coxa-Drehachse -> Femur-Drehachse (waagerecht)
//   femur  Abstand Femur-Drehachse -> Knie-Drehachse
//   tibia  Abstand Knie-Drehachse -> Fußspitze
//   Montage: Position jeder Coxa-Drehachse relativ zur Körpermitte
//            (x nach vorne, y nach links) und die Richtung, in die das Bein
//            bei Coxa 0° zeigt (Grad, gegen den Uhrzeigersinn ab "vorne";
//            links positiv, rechts negativ).

#include "kinematics/kinematics.h"

constexpr bool GEOMETRY_IS_PLACEHOLDER = false;

constexpr kin::LegGeometry LEG_GEOMETRY = {
    41.0f,  // coxa
    43.0f,  // femur
    94.0f,  // tibia
};

// Reihenfolge wie SERVO_MAP: Bein 1-3 links (vorne, Mitte, hinten), 4-6 rechts.
// Coxa-Achsen vorne/hinten 77 mm auseinander, Mitte 114 mm, vorne<->hinten 147 mm.
constexpr kin::LegMount LEG_MOUNTS[6] = {
    {73.5f, 38.5f, 45.0f},      // Bein 1 vorne links
    {0.0f, 57.0f, 90.0f},       // Bein 2 Mitte links
    {-73.5f, 38.5f, 135.0f},    // Bein 3 hinten links
    {73.5f, -38.5f, -45.0f},    // Bein 4 vorne rechts
    {0.0f, -57.0f, -90.0f},     // Bein 5 Mitte rechts
    {-73.5f, -38.5f, -135.0f},  // Bein 6 hinten rechts
};

// Gelenkwinkel (Konvention aus docs/kalibrierung.md), bei denen alle Servos in
// der Mitte stehen (90° / 1500 µs) – so ist das Bein konstruiert: Femur 15°
// nach oben, Tibia senkrecht (also 15° nach innen gegenüber "rechtwinklig").
constexpr kin::JointAngles SERVO_CENTER_POSE = {0.0f, 15.0f, -15.0f};

// Körperhaltung: Standardwerte und Grenzen der Oberfläche
struct PoseLimits {
  float height_min, height_max, height_default;  // Körperhöhe über Boden
  float stance_min, stance_max, stance_default;  // Fußabstand von der Coxa-Achse
  float shift_max;                               // Verschieben x/y
  float tilt_max;                                // Rollen/Nicken
  float yaw_max;                                 // Gieren
};

// Standardhaltung = SERVO_CENTER_POSE:
//   Höhe       = tibia - femur * sin 15° = 82,9 mm
//   Fußabstand = coxa + femur * cos 15° = 82,5 mm
constexpr PoseLimits POSE_LIMITS = {
    50.0f, 115.0f, 82.9f,
    60.0f, 135.0f, 82.5f,
    40.0f,
    20.0f,
    25.0f,
};

// Geschwindigkeit, mit der die Körperhaltung den Vorgaben folgt
constexpr float POSE_SPEED_MM_PER_S = 80.0f;
constexpr float POSE_SPEED_DEG_PER_S = 45.0f;
