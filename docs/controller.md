# Xbox-Controller

Der Hexapod lässt sich außer per Touch/Tastatur auch mit einem Xbox-Controller steuern – auf zwei Wegen
(Seite **Controller** in der Oberfläche):

| Weg | Wie | Vorteil |
|---|---|---|
| **Bluetooth am Roboter** | Controller koppelt direkt mit dem ESP32-S3 | kein Handy nötig |
| **Controller an diesem Gerät** | Controller am Handy/PC (Bluetooth oder USB), die geöffnete Seite leitet ihn weiter | geht mit jedem Controller, den der Browser kennt |

Beide nutzen dieselbe Belegung, die der Roboter auswertet (`src/input/pad_map.cpp`). Touch-Steuerung und
Controller funktionieren gleichzeitig; der Controller sendet nur, wenn an ihm etwas bewegt wird.

## Bluetooth am Roboter

Der ESP32-S3 kann nur **Bluetooth Low Energy** (BLE), kein klassisches Bluetooth. Xbox-Controller der
Series X|S können das ab Werk, der **Elite Series 2** (auch „Core“) und Xbox-One-Controller mit Bluetooth erst
mit aktueller Firmware (seit Ende 2021). Also vorher einmal die App **Xbox Zubehör** (Windows/Xbox) öffnen
und den Controller aktualisieren.

1. *Controller* → Schalter **Bluetooth am Roboter** ein → **Koppeln** (sucht 60 s).
2. Controller einschalten, dann die **Kopplungstaste** (oben neben dem USB-Anschluss) gedrückt halten, bis das
   Xbox-Logo schnell blinkt.
3. Nach ein paar Sekunden: *verbunden: Xbox Wireless Controller · Akku …*, der Controller vibriert kurz.

Danach genügt es, den Controller einzuschalten – der Roboter sucht ihn mit wenig Funklast und verbindet
sich von selbst. **Vergessen** löscht die Kopplung.

Hinweise:

- Ein Xbox-Controller merkt sich **ein** Bluetooth-Gerät (plus eine Xbox). Nach dem Koppeln mit dem Roboter
  muss er mit dem Handy/PC neu gekoppelt werden und umgekehrt (Doppeltipp auf die Kopplungstaste wechselt
  zwischen den zuletzt gekoppelten Geräten).
- Bluetooth und WLAN teilen sich die Antenne. Ist Bluetooth aus, wird es gar nicht gestartet (kein Speicher,
  keine Funkzeit) – dann läuft der Kamerastream am flüssigsten.
- Bricht die Verbindung ab (Controller aus, außer Reichweite), hält der Roboter nach spätestens ca. 1 s an
  (Bluetooth-Timeout; der Controller meldet nur Änderungen, ein gehaltener Stick gilt bis dahin weiter).
- Noch nicht mit echter Hardware getestet: Der Bericht des Controllers wird wie bei der Bibliothek
  [XboxSeriesXControllerESP32](https://github.com/asukiaaa/arduino-XboxSeriesXControllerESP32)
  ausgewertet (Bluetooth-Stack: [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) 2.5.1). Zeigt der
  *Test* auf der Seite verkehrte Stick-Richtungen, hilft *Sticks oben/unten umkehren*.

## Controller an diesem Gerät

Schalter **Controller an diesem Gerät** ein (gilt für diesen Browser). Controller per Bluetooth oder USB mit dem
Handy/PC verbinden und eine Taste drücken – der Browser meldet ihn erst dann. Die Seite schickt den
Zustand alle 50 ms an den Roboter, solange sie geöffnet ist (egal welche Unterseite). Die Xbox-Taste und die
Elite-Paddles meldet der Browser meist nicht.

## Belegung

Die Sticks hängen vom **Modus** ab (Taste Y wechselt, die Steuerung zeigt ihn als 🎮-Abzeichen):

| Modus | linker Stick | rechter Stick | Trigger |
|---|---|---|---|
| Laufen | fahren (vor/zurück, seitlich) | drehen | RT schneller bis 100 %, LT langsamer |
| Körper | verschieben | neigen (rollen/nicken) | LT/RT drehen |
| Vorderbeine | linkes Bein (Fuß folgt dem Stick) | rechtes Bein | RT ausstrecken, LT heranziehen |

In den Modus *Vorderbeine* zu wechseln hebt sie (im Stand), ein anderer Modus setzt sie wieder ab.

Tasten (Standard, unter *Tasten* frei änderbar):

| Taste | Aktion |
|---|---|
| A | Aufstehen / Hinsetzen (Servos aus: erst einschalten, dann noch einmal drücken) |
| B | Stopp (auch Geste beenden) |
| X | Vorderbeine heben / absetzen |
| Y | Modus wechseln (Laufen → Körper → Vorderbeine) |
| LB / RB | Gangart zurück / weiter |
| Steuerkreuz ↑ ↓ | Körper höher / tiefer (5 mm, Halten wiederholt) |
| Steuerkreuz ← → | Tempo − / + (10 %) |
| View | Lebendig an/aus |
| Menü | nächste Geste (Kreisen, Ja, Nein, Liegestütz, Tanzen) |
| Xbox | **Not-Aus** (Freigeben nur in der Oberfläche) |
| LS / RS (drücken) | Lageausgleich an/aus / Winken |
| Share | Haltung zurücksetzen |
| P1 / P2 / P3 / P4 | Modus Laufen / Körper / Vorderbeine / Stopp (Elite-Paddles) |

Weitere wählbare Aktionen: Modus direkt, einzelne Gesten, Servos an/aus. Eine beim Verbinden noch
gedrückte Taste (z. B. die Xbox-Taste zum Einschalten) löst nichts aus.

**Sticks:** *Tempo* ist das Grundtempo beim Laufen, *Totzone* der Ausschlag, der ignoriert wird (gegen
Wandern der Sticks), *Feinfühligkeit* macht kleine Ausschläge feiner (Kennlinie), ohne den Höchstwert zu
ändern.

Meldungen des Controllers (Modus, Gangart, Fehler wie „Erst aufstehen“) erscheinen in der Oberfläche.

## Tests

[`test_host/pad_test.cpp`](../firmware/test_host/pad_test.cpp): Auswertung des Bluetooth-Berichts (Sticks,
Trigger, Tasten, Steuerkreuz, Elite-Paddles), Totzone und Kennlinie, Richtungen je Modus, Flanken und
Wiederholung, Moduswechsel, Belegung.
