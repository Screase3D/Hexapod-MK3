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

// Montagestellung: Gelenkwinkel (Konvention aus docs/kalibrierung.md), in denen
// die Servohörner bei Servo-Mitte (1500 µs) aufgesteckt werden. Die Kalibrierung
// verwendet sie als Bezug (center_us = Puls in dieser Stellung).
// Spinnenartig: Femur 30° nach oben, Tibia senkrecht (= 30° nach innen
// gegenüber "rechtwinklig zum Femur"). Liegt mitten im Arbeitsbereich beim
// Laufen (Femur ca. +5..+90°, Tibia ca. -70..0°), siehe docs/kinematik.md.
constexpr kin::JointAngles ASSEMBLY_POSE = {0.0f, 30.0f, -30.0f};

// Körperhaltung: Standardwerte und Grenzen der Oberfläche
struct PoseLimits {
  float height_min, height_max, height_default;  // Körperhöhe über Boden
  float stance_min, stance_max, stance_default;  // Fußabstand von der Coxa-Achse
  float shift_max;                               // Verschieben x/y
  float tilt_max;                                // Rollen/Nicken
  float yaw_max;                                 // Gieren
};

// Standardhaltung = ASSEMBLY_POSE (alle Servos in der Mitte):
//   Höhe       = tibia - femur * sin 30° = 72,5 mm
//   Fußabstand = coxa + femur * cos 30° = 78,2 mm
constexpr PoseLimits POSE_LIMITS = {
    50.0f, 115.0f, 72.5f,
    60.0f, 135.0f, 78.2f,
    40.0f,
    20.0f,
    25.0f,
};

// Geschwindigkeit, mit der die Körperhaltung den Vorgaben folgt
constexpr float POSE_SPEED_MM_PER_S = 80.0f;
constexpr float POSE_SPEED_DEG_PER_S = 45.0f;
