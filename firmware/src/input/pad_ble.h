#pragma once
// Xbox-Controller direkt per Bluetooth Low Energy (NimBLE). Der ESP32-S3 kann nur
// BLE: Der Controller braucht eine Firmware mit BLE-Unterstützung (Xbox Series X|S
// ab Werk, Elite 2 / One-Controller mit Bluetooth nach dem Update über die
// "Xbox Zubehör"-App).
//
// Ablauf: "Koppeln" sucht 60 s nach einem Controller im Kopplungsmodus (Kopplungs-
// taste gedrückt halten, bis das Xbox-Logo schnell blinkt) und merkt sich ihn.
// Danach wird nur noch mit geringer Funklast nach diesem Controller gesucht und
// bei Bedarf automatisch neu verbunden (Controller einschalten genügt).
// Läuft in einem eigenen Task; alle Funktionen sind thread-sicher.

#include <stdint.h>

#include "pad.h"

namespace padble {

enum class State : uint8_t { Off, Idle, Searching, Pairing, Connecting, Connected };
const char* stateName(State s);  // "off", "idle", "searching", "pairing", "connecting", "connected"

struct Status {
  State state = State::Off;
  char name[32] = "";      // vom Controller gemeldet
  char addr[18] = "";      // gekoppelter Controller (leer = keiner)
  int battery = -1;        // %, -1 = unbekannt
  uint16_t pairing_left_s = 0;
};

// addr: zuletzt gekoppelter Controller ("aa:bb:cc:dd:ee:ff", leer = keiner)
void begin(bool enabled, const char* addr);
void setEnabled(bool on);  // aus = Bluetooth ganz aus (spart Funkzeit fürs WLAN)
void startPairing();
void forget();             // Kopplung löschen, trennen
Status status();

// letzter Zustand des Controllers; false = nicht verbunden
bool latest(pad::State& out);
void rumble(uint8_t strength, uint16_t ms);  // kurze Vibration (Rückmeldung)

// Nach erfolgreichem Koppeln einmal true mit der neuen Adresse (zum Speichern)
bool takePairedAddress(char out[18]);

}  // namespace padble
