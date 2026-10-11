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
// = Grundstellung: Höhe 52 mm (wie im CAD aufgebockt), Fußabstand 100 mm.
// Femur 55,4° nach oben, Linie Knie -> Fußspitze 68° unter der Waagerechten
// (Fuß 35 mm außerhalb des Knies; das Tibia-Bauteil ist gebogen, maßgeblich ist
// nur diese Linie). 100 statt der 110 mm aus dem CAD entlasten den Femur um
// ca. 15 %. Laufen und Sitzen bleiben innerhalb von ±50° um diese Stellung, nur
// der Körper ganz oben (100 mm) braucht -65° am Femur (Standardgrenzen der
// Kalibrierung ±70°), siehe docs/kinematik.md.
constexpr kin::JointAngles ASSEMBLY_POSE = {0.0f, 55.4f, -33.8f};

// Körperhaltung: Standardwerte und Grenzen der Oberfläche
struct PoseLimits {
  float height_min, height_max, height_default;  // Körperhöhe über Boden
  float stance_min, stance_max, stance_default;  // Fußabstand von der Coxa-Achse
  float shift_max;                               // Verschieben x/y
  float tilt_max;                                // Rollen/Nicken
  float yaw_max;                                 // Gieren
};

// Standardhaltung = ASSEMBLY_POSE (alle Servos in der Mitte): Höhe 52 mm
// (Femur-Achse über der Fußspitze; Bodenfreiheit des Chassis dann ca. 28 mm),
// Fußabstand 100 mm. Höhe über 100 mm bringt Femur/Tibia an die Servo-Grenzen.
constexpr PoseLimits POSE_LIMITS = {
    35.0f, 100.0f, 52.0f,
    75.0f, 135.0f, 100.0f,
    40.0f,
    20.0f,
    25.0f,
};

// Geschwindigkeit, mit der die Körperhaltung den Vorgaben folgt
constexpr float POSE_SPEED_MM_PER_S = 80.0f;
constexpr float POSE_SPEED_DEG_PER_S = 45.0f;

// Sitzen (Körper liegt auf) und Aufstehen/Hinsetzen.
// SIT_HEIGHT = Höhe der Femur-Achsen über den Fußspitzen, wenn der Körper flach
// aufliegt. Laut CAD liegt die Chassis-Unterkante 26 mm unter der Femur-Achse,
// die Fußspitze (Ende der 94 mm) ca. 2 mm über dem Boden -> ca. 24 mm.
// Lieber etwas zu klein als zu groß: zu klein -> Füße schweben knapp über dem
// Boden, zu groß -> Füße drücken den Körper beim Einschalten etwas hoch.
// Beim Einschalten von Torque fahren die Servos in diese Haltung.
constexpr float SIT_HEIGHT = 23.0f;
// Gleicher Fußabstand wie im Stand: Aufstehen ist dann ein reines Anheben,
// die Füße müssen nicht umgesetzt werden (Femur 102°, Knie 34°).
// Nicht 101-110 mm: so kleine Unterschiede würden die Füße unter Last schieben,
// erst ab mehr als 10 mm setzt die Gangart sie mit Schritten um.
constexpr float SIT_STANCE = 100.0f;
constexpr float SIT_RISE_S = 1.5f;    // Dauer des Anhebens/Absenkens

// Laufen: Grenzen der Gangparameter in der Oberfläche
struct GaitLimits {
  float swing_min, swing_max, swing_default;     // Dauer eines Fußschwungs (s)
  float height_min, height_max, height_default;  // Fußhub (mm)
  float stride_max;                              // größter Fußweg je Standphase (mm)
};
// Am inneren Ende des Schritts (Fuß nah an der Coxa) muss das Knie zum Anheben
// weit einknicken. Mit 100 mm Fußabstand, 35 mm Schrittlänge und höchstens 20 mm
// Fußhub bleibt der Kniewinkel über ca. 23° (Standard 15 mm: 27°; mit 40 / 25 mm
// wären es 12°). Je höher der Hub und je kürzer der Schritt, desto schneller muss
// der Femur drehen (15 mm / 0,5 s: max. ca. 200 °/s im Schwung, siehe
// test_host/gait_test.cpp).
constexpr GaitLimits GAIT_LIMITS = {0.3f, 1.2f, 0.5f, 5.0f, 20.0f, 15.0f, 35.0f};

// Servo-Rampe, solange die IK die Sollwerte vorgibt: Die Bahnen sind dort schon
// glatt, eine langsamere Rampe würde die Füße nur hinterherhinken lassen.
// (Beim Wechsel in den IK-Modus gilt zunächst die normale Rampe.)
constexpr float GAIT_SERVO_SLEW_DEG_PER_S = 450.0f;
