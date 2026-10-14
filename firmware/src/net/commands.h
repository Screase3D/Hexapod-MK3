#pragma once
// Verarbeitung der Steuerbefehle (WebSocket) und Aufbau der Status-/
// Telemetrie-Nachrichten. Unabhängig vom Transportweg.

#include <ArduinoJson.h>

namespace commands {

// Verarbeitet {"cmd": "...", ...}; Antwort landet in `out` (kann leer bleiben).
void handle(JsonVariantConst in, JsonDocument& out);

void buildTelemetry(JsonDocument& doc);
// {"type":"live", ...} für die Live-Ansicht (siehe docs/api.md)
void buildLive(JsonDocument& doc);
void buildStatus(JsonDocument& doc);

void requestReboot();
bool rebootRequested();

}  // namespace commands
