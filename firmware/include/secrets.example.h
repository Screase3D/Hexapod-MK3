#pragma once
// Optionale Standard-WLAN-Zugangsdaten.
// Diese Datei nach include/secrets.h kopieren und ausfüllen. secrets.h ist in
// .gitignore eingetragen und landet nicht im Repository.
//
// Ohne secrets.h lassen sich die Zugangsdaten auch über die Weboberfläche
// (Tab "System") im Access-Point-Modus eintragen. Dort gespeicherte Daten
// haben Vorrang vor diesen Standardwerten.

#define WIFI_DEFAULT_SSID "MeinWLAN"
#define WIFI_DEFAULT_PASS "geheimes-passwort"

// Optional: Passwort für das Firmware-Update per PlatformIO über WLAN
// (dann in platformio.ini bei hexapod_ota: upload_flags = --auth=...).
// Der Upload über die Weboberfläche ist nicht geschützt – wie die übrige
// Steuerung nur im eigenen WLAN bzw. im Access Point erreichbar.
// #define OTA_PASSWORD "anderes-passwort"
