#pragma once
// I²C-Bus-Scan (0x08–0x77) mit Zuordnung bekannter Geräte.
//
// Der Scan selbst läuft im Regel-Task, weil nur dieser den Bus benutzt.
// Andere Tasks fordern ihn mit request() an und lesen das Ergebnis mit
// toJson(). Adressen ohne Antwort erzeugen keine Log-Meldungen (Probe).

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Wire.h>

namespace i2cscan {

// Sofort scannen (nur aufrufen, solange kein anderer Task den Bus nutzt,
// z.B. in setup()) und das Ergebnis auf Serial ausgeben.
void scanNow(TwoWire& wire);

// Scan anfordern; liefert die Nummer des Scans, auf den gewartet werden kann.
uint32_t request();
// Aus dem Regel-Task aufrufen: führt einen angeforderten Scan aus.
void process(TwoWire& wire);
// Nummer des zuletzt abgeschlossenen Scans
uint32_t completed();

void toJson(JsonObject obj);

}  // namespace i2cscan
