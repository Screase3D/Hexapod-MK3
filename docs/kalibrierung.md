# Servo-Kalibrierung

Die inverse Kinematik rechnet in **Gelenkwinkeln**. Die Kalibrierung legt für jeden der
18 Servos fest, welche Pulsbreite zu welchem Winkel gehört:

```
puls_us = center_us + winkel_deg × us_per_deg        (begrenzt auf min_deg … max_deg)
```

| Wert | Bedeutung |
|---|---|
| `center_us` | Pulsbreite in der Neutralstellung (Gelenkwinkel 0°) |
| `us_per_deg` | Steigung; das **Vorzeichen** ist die Drehrichtung (MG90S ≈ ±11,1 µs/°) |
| `min_deg`, `max_deg` | Software-Endanschläge des Gelenks |

Die Werte liegen im NVS des ESP32 (überleben Neustart und Firmware-Update) und lassen sich im
Tab **System** als JSON exportieren – am besten eine Kopie ins Repository legen.

## Gelenk-Konvention

Gilt für **beide Körperseiten gleich**, damit links und rechts dieselben Zahlen bedeuten.

### Neutralstellung (alle Gelenke 0°)

Draufsicht (vorne = oben) – jede Coxa zeigt entlang ihrer Montageachse gerade vom Körper weg:

```
            ▲ VORNE
   Bein 1 ──┐     ┌── Bein 4
            │     │
   Bein 2 ──┤     ├── Bein 5
            │     │
   Bein 3 ──┘     └── Bein 6
```

Seitenansicht eines Beins – Femur waagerecht, Tibia im rechten Winkel dazu (senkrecht nach unten):

```
   Körper ┃
          ┃═●━━━━━━━━━━━●   ● = Gelenkachsen (Femur-Gelenk, Knie)
          ┃   Femur     ┃
                        ┃ Tibia
                        ┃
                        ▼ Fuß
```

> **Hexapod MK3:** Bei Servo-Mitte (1500 µs) steht der Femur konstruktionsbedingt 15° nach oben
> und die Tibia senkrecht – das ist **nicht** die 0°-Stellung oben, sondern Femur +15°,
> Tibia −15° ([Zeichnung](beingeometrie.png)). Für die Kalibrierung den Femur auf waagerecht und
> die Tibia auf rechtwinklig zum Femur fahren (siehe Ablauf, Schritt 2).

### Positive Drehrichtung

| Gelenk | 0° | positiver Winkel |
|---|---|---|
| **Coxa** | Bein zeigt entlang der Montageachse vom Körper weg | Fuß schwenkt nach **vorne** |
| **Femur** | Oberschenkel waagerecht | Oberschenkel hebt sich (**nach oben**) |
| **Tibia** | Unterschenkel 90° zum Femur (bei waagerechtem Femur senkrecht nach unten) | Fuß bewegt sich nach **außen** (Knie öffnet sich) |

„Tibia“ meint die Linie von der Kniegelenkachse zur Fußspitze – bei gebogenen Unterschenkeln
also nicht die Außenkante des Bauteils.

## Ablauf

> Vor dem ersten Einschalten: Roboter aufbocken, sodass die Beine frei hängen.
> Not-Aus: roter **STOP**-Knopf oder Taste **Esc**.

1. **Montagestellung.** Servohörner abnehmen. Im Tab *Kalibrierung* **Torque** einschalten und
   **„Alle auf 1500 µs (Montage)“** drücken. Jetzt die Hörner/Beinteile so aufstecken, dass jedes
   Gelenk so nah wie möglich an der konstruierten Montagestellung steht (MK3: Coxa in
   Montagerichtung, Femur 15° nach oben, Tibia senkrecht). Die Verzahnung erlaubt nur
   Schritte von einigen Grad – den Rest erledigt Schritt 2.
2. **0° feinjustieren.** Ein Gelenk in der Beinübersicht anklicken, im Rohmodus
   mit ±1/±10 µs (oder Pfeiltasten ←/→, mit Umschalt ±10) exakt auf 0° fahren und
   **„Aktuelle Stellung = 0°“** drücken. Beim MK3 heißt das: Femur um 15° **absenken** bis
   waagerecht (≈ 170 µs), Tibia so weit drehen, dass sie rechtwinklig zum Femur steht.
   Je Bein **zuerst den Femur** kalibrieren – bei waagerechtem Femur steht die Tibia dann
   einfach senkrecht.
   Tipp: Femur waagerecht und Tibia senkrecht lassen sich gut mit einer Wasserwaagen-App auf
   dem Handy oder einem Winkel prüfen.
3. **Richtung und Maßstab.** Das Gelenk auf einen bekannten Winkel fahren (z. B. 45° mit einer
   gedruckten Winkellehre), den Winkel **mit Vorzeichen laut Tabelle oben** eintragen und
   **„Aktuelle Stellung = Referenz“** drücken. Daraus ergibt sich `us_per_deg` inklusive
   Drehrichtung. Wer diesen Schritt auslässt, prüft die Richtung mit dem Test-Schieber und nutzt
   bei Bedarf **„Drehrichtung umkehren“**.
4. **Grenzen.** Mit dem Test-Schieber (Abschnitt 3, kalibriert) langsam bis kurz vor
   mechanische Kollisionen fahren und `Min`/`Max` eintragen → **„Werte übernehmen“**.
5. Für alle 18 Servos wiederholen (↑/↓ springt zum vorherigen/nächsten Servo). Kalibrierte
   Gelenke bekommen in der Übersicht einen grünen Punkt.
6. **Speichern** – erst dann landen die Werte dauerhaft im NVS. „Verwerfen“ lädt den letzten
   gespeicherten Stand, „Standardwerte“ setzt alles zurück (ebenfalls erst mit Speichern
   dauerhaft).
7. Im Tab *System* **Exportieren** und die JSON-Datei sichern.

## Hinweise

- Der **Rohmodus ignoriert die Grenzen** (nur 500–2500 µs absolut). Langsam fahren, solange die
  Beine montiert sind.
- Alle Bewegungen laufen über eine Rampe (Standard 240 °/s, einstellbar im Tab *Steuerung*).
- Nach dem Einschalten fahren die Servos beim Aktivieren von Torque direkt auf die
  Neutralstellung (die echte Position ist vorher unbekannt). Der Roboter sollte dabei
  aufgebockt sein oder frei liegen.
- Import einer JSON-Datei überschreibt alle 18 Servos und speichert sofort.
