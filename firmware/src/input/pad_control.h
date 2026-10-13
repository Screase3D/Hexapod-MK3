#pragma once
// Controller-Steuerung: nimmt den Gamepad-Zustand vom Bluetooth-Controller (pad_ble)
// oder aus dem Browser (Befehl "pad", Gamepad am Handy/PC), setzt ihn über die
// Belegung (pad_map) in Bewegungsbefehle um und führt die Tasten-Aktionen aus.
// Touch-Steuerung und Controller gehen beide; gesendet wird nur, wenn sich am
// Controller etwas bewegt. Einstellungen im NVS.

#include <Arduino.h>
#include <ArduinoJson.h>

#include "pad.h"

namespace padctl {

void begin();  // Einstellungen laden, Bluetooth starten (falls eingeschaltet)
void loop();   // aus loop(), wertet etwa alle 20 ms aus

void webInput(const pad::State& s);  // Gamepad im Browser (ca. 20 Hz, sonst gilt er als getrennt)

void configToJson(JsonObject obj);
bool applyJson(JsonVariantConst in, String& err);
void telemetry(JsonObject obj);

}  // namespace padctl
