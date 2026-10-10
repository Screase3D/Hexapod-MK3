#pragma once
// HTTP-Server auf Port 80: Weboberfläche, REST-API und WebSocket (/ws).
//
// REST (JSON):
//   GET  /api/status            Systemstatus
//   GET  /api/calibration       komplette Servokalibrierung
//   POST /api/calibration       Kalibrierung importieren (wird sofort gespeichert)
//   GET  /api/camera            Kameraeinstellungen
//   POST /api/camera            Kameraeinstellungen ändern (persistent)
//   GET  /api/wifi              WLAN-Status
//   POST /api/wifi              {"ssid","pass"} speichern | {"clear":true}
//   POST /api/estop             Not-Aus (Fallback, falls WebSocket hängt)
//   POST /api/reboot            Neustart
//
// WebSocket /ws: Steuerbefehle als JSON {"cmd": ...}, siehe docs/api.md.
// Der Server schickt zyklisch {"type":"telemetry", ...} an alle Clients.

#include <Arduino.h>

namespace web {

bool begin();
// Sendet eine Textnachricht an alle verbundenen WebSocket-Clients.
void broadcast(const String& msg);
int wsClientCount();
// Zeit seit der letzten Nachricht eines Clients (für Steuer-Watchdog)
uint32_t msSinceLastCommand();

}  // namespace web
