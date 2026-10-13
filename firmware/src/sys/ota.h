#pragma once
// Firmware-Update über WLAN, zwei Wege:
//  - Weboberfläche: Einstellungen -> Firmware-Update, .bin hochladen (POST /api/ota)
//  - PlatformIO:    pio run -e hexapod_ota -t upload  (ArduinoOTA, Port 3232)
// Vor dem Schreiben werden die Servos stromlos geschaltet. Nach Erfolg startet der
// ESP32 neu und läuft mit der neuen Firmware (zweite App-Partition).

#include <Arduino.h>

namespace ota {

void begin();  // ArduinoOTA starten (nach dem WLAN)
void loop();

// Web-Upload in Stücken: begin -> write ... -> end. false = Fehler (err)
bool webBegin(size_t size, String& err);
bool webWrite(const uint8_t* data, size_t len, String& err);
bool webEnd(String& err);
void webAbort();
bool active();  // Update läuft gerade

}  // namespace ota
