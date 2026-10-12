#pragma once
// Zuordnung Bein/Gelenk -> PCA9685-Board/Kanal (laut Schaltplan v4).
//
//   Servo-ID = Bein * 3 + Gelenk
//   Bein 0..2 = linke Seite (vorne, Mitte, hinten)  -> PCA #2 (0x41), CH0..CH8
//   Bein 3..5 = rechte Seite (vorne, Mitte, hinten) -> PCA #1 (0x40), CH0..CH8
//   (Die Platinen sind gespiegelt eingebaut: #1 sitzt rechts, #2 links.)
//   Gelenk 0 = Coxa (Hüfte), 1 = Femur (Oberschenkel), 2 = Tibia (Unterschenkel)
//
// In der Oberfläche werden die Beine wie im Schaltplan 1..6 nummeriert.

#include <stdint.h>
#include "config.h"

enum Joint : uint8_t { JOINT_COXA = 0, JOINT_FEMUR = 1, JOINT_TIBIA = 2 };

struct ServoMapEntry {
  uint8_t board;    // 0 = PCA #1 (0x40, rechts), 1 = PCA #2 (0x41, links)
  uint8_t channel;  // 0..15
};

constexpr ServoMapEntry SERVO_MAP[NUM_SERVOS] = {
    // Bein 1 - vorne links  (PCA #2)
    {1, 0}, {1, 1}, {1, 2},
    // Bein 2 - Mitte links   (PCA #2)
    {1, 3}, {1, 4}, {1, 5},
    // Bein 3 - hinten links  (PCA #2)
    {1, 6}, {1, 7}, {1, 8},
    // Bein 4 - vorne rechts  (PCA #1)
    {0, 0}, {0, 1}, {0, 2},
    // Bein 5 - Mitte rechts  (PCA #1)
    {0, 3}, {0, 4}, {0, 5},
    // Bein 6 - hinten rechts (PCA #1)
    {0, 6}, {0, 7}, {0, 8},
};

constexpr int NUM_BOARDS = 2;

inline constexpr int servoId(int leg, int joint) { return leg * JOINTS_PER_LEG + joint; }
inline constexpr int servoLeg(int id) { return id / JOINTS_PER_LEG; }
inline constexpr int servoJoint(int id) { return id % JOINTS_PER_LEG; }
inline constexpr bool legIsLeft(int leg) { return leg < 3; }

// Drehrichtung ohne Kalibrierung (+1: steigender Puls -> steigender Gelenkwinkel,
// Konvention siehe docs/kalibrierung.md). Die Beine sind links und rechts
// spiegelbildlich aufgebaut, links dreht jedes Gelenk bei gleichem Puls andersherum:
//  - Femur/Tibia: Servos gespiegelt eingebaut (rechts: Puls hoch = Femur hoch).
//  - Coxa: alle Servos mit der Welle nach oben (drehen von oben gesehen gleich),
//    "+ = Fuß nach vorne" ist aber rechts gegen, links im Uhrzeigersinn.
// Der Kalibrierschritt "Richtung" bzw. "Drehrichtung umkehren" überschreibt das je Servo.
constexpr int8_t SERVO_DEFAULT_DIR[2][JOINTS_PER_LEG] = {
    // Coxa, Femur, Tibia
    {-1, -1, -1},  // links (Beine 1-3)
    {+1, +1, +1},  // rechts (Beine 4-6)
};
inline constexpr int servoDefaultDir(int id) {
  return SERVO_DEFAULT_DIR[legIsLeft(servoLeg(id)) ? 0 : 1][servoJoint(id)];
}

inline const char* legName(int leg) {
  static const char* const names[NUM_LEGS] = {
      "vorne links", "Mitte links", "hinten links",
      "vorne rechts", "Mitte rechts", "hinten rechts"};
  return (leg >= 0 && leg < NUM_LEGS) ? names[leg] : "?";
}

inline const char* jointName(int joint) {
  static const char* const names[JOINTS_PER_LEG] = {"Coxa", "Femur", "Tibia"};
  return (joint >= 0 && joint < JOINTS_PER_LEG) ? names[joint] : "?";
}
