# Servo-Kalibrierung

Die inverse Kinematik rechnet in **Gelenkwinkeln**. Die Kalibrierung legt für jeden der
18 Servos fest, welche Pulsbreite zu welchem Winkel gehört. Bezugspunkt ist die
**Montagestellung** – die Stellung, in der die Servohörner bei Servo-Mitte (1500 µs)
aufgesteckt werden:

```
puls_us = center_us + (winkel_deg − ref_deg) × us_per_deg     (begrenzt auf min_deg … max_deg)
```

| Wert | Bedeutung |
|---|---|
| `ref_deg` | Gelenkwinkel der Montagestellung, fest je Gelenktyp (`ASSEMBLY_POSE` in `robot_geometry.h`): Coxa 0°, Femur +30°, Tibia −30° |
| `center_us` | Pulsbreite in der Montagestellung (nach dem Aufstecken ≈ 1500 µs) |
| `us_per_deg` | Steigung; das **Vorzeichen** ist die Drehrichtung (MG90S ≈ ±11,1 µs/°) |
| `min_deg`, `max_deg` | Software-Endanschläge des Gelenks (Gelenkwinkel) |

Die Werte liegen im NVS des ESP32 (überleben Neustart und Firmware-Update) und lassen sich im
Tab **System** als JSON exportieren – am besten eine Kopie ins Repository legen.

## Montagestellung

Der Hexapod steht „spinnenartig“: Femur deutlich nach oben, Tibia etwa senkrecht. Deshalb
werden die Hörner nicht bei 0° (Femur waagerecht), sondern in dieser Haltung aufgesteckt:

| Gelenk | Montagestellung | Gelenkwinkel |
|---|---|---|
| Coxa | Bein zeigt in Montagerichtung (Ecken 45°, Mitte 90°) | 0° |
| Femur | 30° nach oben | +30° |
| Tibia | senkrecht nach unten (30° nach innen ggü. „rechtwinklig zum Femur“) | −30° |

So liegt die Servo-Mitte in der Mitte des Bereichs, den das Bein beim Laufen braucht
(Femur etwa +5…+90°, Tibia etwa −70…0°, siehe [kinematik.md](kinematik.md)); jeder Servo hat
nach beiden Seiten genug Weg. Die Montagestellung ist zugleich die **Standardhaltung** der IK
(Körperhöhe 72,5 mm, Fußabstand 78,2 mm) – „Alle auf Montagestellung“ stellt den Roboter hin.

Zum Aufstecken und Prüfen gibt es eine **Lehre zum Ausdrucken**:
[montagelehre.pdf](montagelehre.pdf) (A4, mit 100 % drucken, Kontrollmaß 50 mm nachmessen).
Sie zeigt die Achslinien in Originalgröße: Femur-Achse und Knie-Achse als Kreuze, die
Fußspitze als Marke und eine Winkellehre für die Coxa. Wer es genauer will, konstruiert im CAD
kleine Keile, die zwischen Coxa-Halter und Femur bzw. Femur und Tibia genau 30° bzw. 60°
(Kniewinkel) einstellen.

## Gelenk-Konvention

Gilt für **beide Körperseiten gleich**, damit links und rechts dieselben Zahlen bedeuten.

### Nullstellung (alle Gelenke 0°)

Die Winkel-Konvention ist reine Mathematik; die Montagestellung oben hat andere Winkel.

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

1. **Hörner aufstecken.** Servohörner abnehmen. Im Tab *Kalibrierung* **Torque** einschalten und
   **„Alle auf 1500 µs (Montage)“** drücken. Jetzt die Hörner/Beinteile so aufstecken, dass jedes
   Bein so nah wie möglich an der Montagestellung steht (Montagelehre). Die Verzahnung erlaubt
   nur Schritte von einem Zahn (≈ 18°) – also den Zahn wählen, bei dem die Abweichung am
   kleinsten ist; den Rest erledigt Schritt 2.
2. **Montagestellung feinjustieren.** Ein Gelenk in der Beinübersicht anklicken, im Rohmodus
   mit ±1/±10 µs (oder Pfeiltasten ←/→, mit Umschalt ±10) exakt in die Montagestellung fahren
   und **„Aktuelle Stellung = Montagestellung“** drücken. Je Bein **zuerst den Femur**, dann
   die Tibia (die Lehre richtet sich an Femur- und Knie-Achse aus).
3. **Richtung und Maßstab.** Das Gelenk auf einen zweiten bekannten Winkel fahren, den Winkel
   **mit Vorzeichen laut Tabelle oben** eintragen und **„Aktuelle Stellung = Referenz“**
   drücken. Daraus ergibt sich `us_per_deg` inklusive Drehrichtung. Gut messbar sind:
   - Coxa ±45°: Eckbeine stehen dann parallel bzw. quer zur Körperlängsachse, Mittelbeine
     parallel zu den Eckbeinen in Montagestellung (Winkellehre C),
   - Femur 0°: waagerecht (rote Linie der Lehre, Wasserwaagen-App),
   - Tibia 0°: rechtwinklig zum Femur (blaue Linie der Lehre).

   Wer diesen Schritt auslässt, prüft die Richtung mit dem Test-Schieber und nutzt bei Bedarf
   **„Drehrichtung umkehren“** – die Montagestellung bleibt dabei erhalten.
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
- Nach dem Einschalten fahren die Servos beim Aktivieren von Torque direkt in die
  Sitzhaltung (Füße weit außen, siehe [kinematik.md](kinematik.md#aufstehen-und-hinsetzen); die
  echte Position ist vorher unbekannt). Für die Montage danach „Alle auf 1500 µs (Montage)“.
- Import einer JSON-Datei überschreibt alle 18 Servos und speichert sofort.
- Kalibrierungen aus älteren Firmware-Ständen (Bezug 0°, Version 1) werden beim Laden und beim
  Import automatisch auf die Montagestellung umgerechnet; nie kalibrierte Servos (1500 µs,
  Standardsteigung) bleiben Standardwerte. Nach dem Umstecken der Hörner trotzdem neu kalibrieren.
- Wird `ASSEMBLY_POSE` in `robot_geometry.h` geändert, müssen die Hörner neu ausgerichtet und
  Schritt 2 wiederholt werden.
