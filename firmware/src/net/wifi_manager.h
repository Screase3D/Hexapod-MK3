#pragma once
// WLAN-Verwaltung.
//
// Start: Mit gespeicherten Zugangsdaten verbindet sich der Hexapod mit dem
// Heimnetz (Station). Ohne Zugangsdaten oder wenn das fehlschlägt, startet der
// Access Point "Hexapod-MK3" (192.168.4.1) mit Captive Portal, sodass sich die
// Oberfläche beim Verbinden automatisch öffnet.
//
// Einrichtung über die Oberfläche: Netze suchen, eins auswählen, Passwort
// eingeben -> der ESP verbindet sich sofort, während der Access Point noch
// läuft, und meldet seine neue IP. Danach schaltet sich der Access Point ab.
//
// Alle WLAN-Aktionen laufen in loop(); die Web-Handler stellen nur Anfragen.

#include <Arduino.h>
#include <ArduinoJson.h>

namespace wifi {

void begin();
void loop();  // regelmäßig aus Arduino-loop() aufrufen

bool isAccessPoint();  // Access Point aktiv

// Bluetooth an/aus (pad_ble): schaltet den WLAN-Energiesparmodus um. Mit
// Bluetooth muss das WLAN Modem-Sleep nutzen (Antenne wird geteilt), ohne bleibt
// er für die geringste Latenz aus.
void setBluetoothActive(bool on);
bool bluetoothActive();
int lastDisconnectReason();             // letzter Trennungsgrund (WIFI_REASON_*)
const char* disconnectText(int reason);  // Klartext (nie nullptr)
int lastConnectTries();     // Anläufe bis zur letzten Verbindung (1 = sofort)
uint32_t lastConnectMs();   // Dauer bis zur letzten Verbindung
String failReason();        // warum der letzte Verbindungsversuch scheiterte
uint32_t nextRetryS();      // AP-Modus: Sekunden bis zum nächsten Versuch (0 = keiner geplant/läuft)
bool staConnected();   // mit dem Heimnetz verbunden
String ip();           // Heimnetz-IP, sonst Access-Point-IP
void toJson(JsonObject obj);

// Netzsuche anfordern bzw. Ergebnis abfragen
void requestScan();
void scanToJson(JsonObject obj);

// Zugangsdaten prüfen und speichern. connect_now: im Access-Point-Modus sofort
// verbinden; im Heimnetz-Betrieb gelten neue Daten erst nach einem Neustart.
bool configure(const String& ssid, const String& pass, bool connect_now, String& msg);
void clearCredentials();
String savedSsid();

}  // namespace wifi
