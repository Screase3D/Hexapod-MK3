#pragma once
// WLAN: verbindet sich mit dem gespeicherten Netz (Station). Gelingt das
// nicht, startet ein eigener Access Point "Hexapod-MK3" (192.168.4.1).
// In beiden Fällen ist die Oberfläche zusätzlich über http://hexapod.local
// erreichbar (mDNS; im AP-Modus nicht auf allen Geräten).

#include <Arduino.h>
#include <ArduinoJson.h>

namespace wifi {

void begin();
void loop();

bool isAccessPoint();
bool connected();
String ip();
void toJson(JsonObject obj);

// Speichert Zugangsdaten im NVS; wirksam nach Neustart.
bool saveCredentials(const String& ssid, const String& pass);
void clearCredentials();
String savedSsid();

}  // namespace wifi
