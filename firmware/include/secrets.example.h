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
