#pragma once
// Zuordnung Bein/Gelenk -> PCA9685-Board/Kanal (laut Schaltplan v3).
//
//   Servo-ID = Bein * 3 + Gelenk
//   Bein 0..2 = linke Seite (vorne, Mitte, hinten)  -> PCA #1 (0x40), CH0..CH8
//   Bein 3..5 = rechte Seite (vorne, Mitte, hinten) -> PCA #2 (0x41), CH0..CH8
//   Gelenk 0 = Coxa (Hüfte), 1 = Femur (Oberschenkel), 2 = Tibia (Unterschenkel)
//
// In der Oberfläche werden die Beine wie im Schaltplan 1..6 nummeriert.

#include <stdint.h>
#include "config.h"

enum Joint : uint8_t { JOINT_COXA = 0, JOINT_FEMUR = 1, JOINT_TIBIA = 2 };

struct ServoMapEntry {
  uint8_t board;    // 0 = PCA #1, 1 = PCA #2
  uint8_t channel;  // 0..15
};

constexpr ServoMapEntry SERVO_MAP[NUM_SERVOS] = {
    // Bein 1 - vorne links
    {0, 0}, {0, 1}, {0, 2},
    // Bein 2 - Mitte links
    {0, 3}, {0, 4}, {0, 5},
    // Bein 3 - hinten links
    {0, 6}, {0, 7}, {0, 8},
    // Bein 4 - vorne rechts
    {1, 0}, {1, 1}, {1, 2},
    // Bein 5 - Mitte rechts
    {1, 3}, {1, 4}, {1, 5},
    // Bein 6 - hinten rechts
    {1, 6}, {1, 7}, {1, 8},
};

constexpr int NUM_BOARDS = 2;
constexpr int CHANNELS_PER_BOARD = 9;  // CH0..CH8 belegt

inline constexpr int servoId(int leg, int joint) { return leg * JOINTS_PER_LEG + joint; }
inline constexpr int servoLeg(int id) { return id / JOINTS_PER_LEG; }
inline constexpr int servoJoint(int id) { return id % JOINTS_PER_LEG; }
inline constexpr bool legIsLeft(int leg) { return leg < 3; }

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
