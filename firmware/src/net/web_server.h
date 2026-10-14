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
//   POST /api/wifi              {"ssid","pass","connect"} speichern (+ verbinden) | {"clear":true}
//   GET  /api/wifi/scan         Ergebnis der Netzsuche
//   POST /api/wifi/scan         Netzsuche starten
//   GET  /api/battery           Akkuwerte + Schalter
//   POST /api/battery           {"enabled","deep_sleep"} Überwachung/Tiefschlaf schalten
//   GET  /api/kinematics        Beinmaße, Montagepunkte, Grenzen der Körperhaltung
//   GET  /api/i2c               Ergebnis des letzten I²C-Scans
//   POST /api/i2c/scan          I²C-Bus neu scannen
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
// Live-Ansicht: hat ein Client in den letzten LIVE_HOLD_MS {"cmd":"live"} geschickt?
bool liveWanted();
// Sendet nur an diese Clients
void broadcastLive(const String& msg);
int wsClientCount();
// Zeit seit der letzten Nachricht eines Clients (für Steuer-Watchdog)
uint32_t msSinceLastCommand();

}  // namespace web
