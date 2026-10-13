#pragma once
// Gamepad-Zustand (Xbox-Layout) und Auswertung des Bluetooth-Berichts – reine
// Logik ohne Hardware (Tests: test_host/pad_test.cpp).

#include <stddef.h>
#include <stdint.h>

namespace pad {

// Tasten als Bits (Reihenfolge = Index für Belegung und Namen)
enum Button : uint8_t {
  BTN_A, BTN_B, BTN_X, BTN_Y, BTN_LB, BTN_RB, BTN_VIEW, BTN_MENU, BTN_XBOX, BTN_LS, BTN_RS,
  BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_SHARE, BTN_P1, BTN_P2, BTN_P3, BTN_P4,
  NUM_BUTTONS
};
const char* buttonName(int b);  // "a", "b", "x", "y", "lb", "rb", "view", "menu", "xbox", "ls", "rs",
                                // "up", "down", "left", "right", "share", "p1" .. "p4"
int buttonFromName(const char* name);  // -1 = unbekannt
constexpr uint32_t mask(int b) { return 1u << b; }

struct State {
  float lx = 0, ly = 0;  // linker Stick, -1..1, x rechts +, y oben +
  float rx = 0, ry = 0;  // rechter Stick
  float lt = 0, rt = 0;  // Trigger 0..1
  uint32_t buttons = 0;  // Bits nach Button
};

// HID-Bericht eines Xbox-Controllers über BLE (Series X|S, One mit BLE-Firmware,
// Elite 2): 16 Bytes, beim Elite 2 länger (Profil Byte 16, Paddles Byte 18).
// false = zu kurz.
bool parseXboxReport(const uint8_t* data, size_t len, State& out);

}  // namespace pad
