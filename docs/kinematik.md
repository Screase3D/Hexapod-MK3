# Kinematik und Körperhaltung

Die inverse Kinematik (IK) rechnet aus gewünschten Fußpunkten die 18 Gelenkwinkel. Damit das
am echten Roboter stimmt, braucht sie die **Maße der Beine und ihre Montagepunkte am Körper**.
Diese stehen in [`firmware/include/robot_geometry.h`](../firmware/include/robot_geometry.h) und
stammen aus dem CAD-Modell:

![Beingeometrie](beingeometrie.png)

| | Wert |
|---|---|
| Coxa / Femur / Tibia | 41 / 43 / 94 mm (Achse zu Achse, Tibia bis Fußspitze) |
| Coxa-Achsen vorne bzw. hinten | 77 mm auseinander, ±73,5 mm vor/hinter der Mitte, Bein 45° schräg |
| Coxa-Achsen Mitte | 114 mm auseinander, Bein 90° zur Seite |
| Montagestellung (alle Servos 90°) | Coxa 0°, Femur +49,3°, Tibia −23,5° (Grundstellung aus dem CAD) |
| daraus Standardhaltung | Höhe 52 mm (Femur-Achse über der Fußspitze), Fußabstand 110 mm, Bodenfreiheit ca. 28 mm |

Die Zeichnung wird von [`beingeometrie.py`](beingeometrie.py) erzeugt
(`python3 docs/beingeometrie.py`); ändern sich Maße, dort und in `robot_geometry.h` anpassen.

### Montagestellung (Servo-Mitte) = Grundstellung

Die Servohörner werden bei Servo-Mitte (90° / 1500 µs) in der **Grundstellung** aufgesteckt,
in der der Roboter auch steht. Sie stammt aus dem CAD (Seitenansicht des Beins, Roboter
aufgebockt):

| | Wert |
|---|---|
| Femur | 49,3° über der Waagerechten |
| Linie Knieachse → Fußspitze | 64,2° unter der Waagerechten, Fußspitze 41 mm außerhalb des Knies |
| Gelenkwinkel (Firmware) | **Coxa 0°, Femur +49,3°, Tibia −23,5°** (`ASSEMBLY_POSE`) |
| Höhe / Fußabstand | 52 mm (Femur-Achse über der Fußspitze) / 110 mm |
| Bodenfreiheit Chassis | ca. 28 mm (Unterkante 26 mm unter der Femur-Achse, runder Fuß ca. 2 mm) |

Das Tibia-Bauteil ist gebogen: Vom Knie geht es erst nach außen und knickt dann nach unten ab,
die Fußspitze steht etwa unter diesem Knick. Für die IK zählt nur die gerade Linie von der
Knieachse zur Fußspitze (94 mm); die Form spielt nur für Anschläge eine Rolle.

Die Kalibrierung verwendet diese Stellung als Bezug, und die Standardhaltung der Oberfläche
entspricht ihr genau. Eine Lehre zum Ausdrucken: [montagelehre.pdf](montagelehre.pdf).

**Warum diese Stellung als Servo-Mitte?** Sie liegt mitten im Arbeitsbereich – alle Haltungen
bleiben innerhalb von ±60° um sie (MG90S: etwa ±90°):

| Haltung | Femur | Tibia | Knie innen |
|---|---|---|---|
| Grundstellung 52 / 110 mm | 49° (±0) | −24° (±0) | 67° |
| Fuß 20 mm angehoben | 76° (+26) | −37° (−14) | 53° |
| Liegen (Sitzhaltung 23 / 110 mm) | 87° (+38) | −42° (−18) | 48° |
| Körper hoch 80 / 100 mm | 17° (−33) | −6° (+18) | 84° |
| Körper sehr hoch 100 / 100 mm | −10° (−59) | +20° (+44) | 110° |

Außerdem ist sie gut einzustellen: 1 mm Fehler in der Höhe macht nur ca. 1,3° am Femur.

**Last:** Das Femur-Moment hängt vom waagerechten Abstand Femur-Achse → Fuß ab (in der
Grundstellung 69 mm). Bei ca. 850 g und drei tragenden Beinen (Dreifuß-Gang) sind das etwa
2 kg·cm – so viel, wie ein MG90S gerade noch hält. Im Stand auf sechs Beinen ist es die Hälfte.
Wird der Femur beim Laufen zu schwach: Fußabstand verkleinern (95 mm → ca. 1,5 kg·cm,
85 mm → ca. 1,25 kg·cm) oder Ripple-Gang (vier Beine tragen). Am besten den fertigen Roboter
einmal wiegen.

Voraussetzung: Die Servos sind kalibriert ([kalibrierung.md](kalibrierung.md)), denn die IK
verwendet dieselben Gelenkwinkel-Konventionen.

## Bedeutung der Maße

Alle Maße in mm, jeweils zwischen den **Drehachsen** der Servos (Mitte der Abtriebswelle):

```
 Seitenansicht eines Beins bei 0° (Femur waagerecht, Tibia rechtwinklig)

   Coxa-Achse     Femur-Achse          Knie-Achse
       │◄── coxa ──►│◄────── femur ──────►│
       ●────────────●━━━━━━━━━━━━━━━━━━━━●
                                         ┃
                                         ┃ tibia (bis zur Fußspitze)
                                         ┃
                                         ▼
```

| Wert | Messung |
|---|---|
| `coxa` | waagerechter Abstand Coxa-Drehachse → Femur-Drehachse |
| `femur` | Abstand Femur-Drehachse → Knie-Drehachse |
| `tibia` | Abstand Knie-Drehachse → Fußspitze (Aufsetzpunkt) |

Für **jedes der 6 Beine** zusätzlich (in `LEG_MOUNTS`):

| Wert | Messung |
|---|---|
| `x` | Coxa-Drehachse vor (+) bzw. hinter (−) der Körpermitte |
| `y` | Coxa-Drehachse links (+) bzw. rechts (−) der Körpermitte |
| `angle_deg` | Richtung, in die das Bein bei Coxa 0° zeigt: 0° = nach vorne, 90° = nach links, −90° = nach rechts, ±180° = nach hinten |

Bei einem symmetrischen Körper genügen die Werte für ein Bein vorne, eins in der Mitte und eins
hinten; die rechte Seite hat dann `y` und `angle_deg` mit umgekehrtem Vorzeichen. Hat das Bein
einen Höhenversatz zwischen Coxa- und Femur-Achse oder ist die Tibia stark gebogen, bitte
melden – das Modell lässt sich erweitern.

`GEOMETRY_IS_PLACEHOLDER = true` blendet in der Oberfläche einen Warnhinweis ein (für einen
Umbau mit noch unbekannten Maßen).

Der Montagewinkel der Eckbeine (45°) ist eine Festlegung: Bei Coxa 0° zeigt das Bein genau
diagonal, das stellt die Kalibrierung ein. Weicht der Winkel am Körper deutlich ab (z. B. weil
die Coxa-Servos schräg eingebaut sind), `angle_deg` in `LEG_MOUNTS` anpassen.

## Koordinatensysteme

| System | Ursprung | Achsen |
|---|---|---|
| Welt | Boden unter der Körpermitte (Nullpose) | x vorne, y links, z oben |
| Körper | Körpermitte auf Höhe der Coxa-Achsen | x vorne, y links, z oben |
| Bein | Coxa-Achse | x in Montagerichtung, y 90° gegen den Uhrzeigersinn, z oben |

## Körperhaltung in der Oberfläche

Tab *Steuerung* → **Körperhaltung (IK)** → *aktiv*. Die Füße bleiben auf ihren Standardpunkten
(in Montagerichtung, im Abstand *Fußabstand* von der Coxa-Achse), der Körper bewegt sich
darüber:

| Regler | Bedeutung |
|---|---|
| Höhe | Femur-Achsen über den Fußspitzen (35–100 mm, Standard 52) |
| Fußabstand | Abstand Fuß ↔ Coxa-Achse (waagerecht, 75–135 mm, Standard 110) |
| Vor/zurück, Links/rechts | Körper über den Füßen verschieben |
| Rollen | + = rechte Seite runter |
| Nicken | + = Nase hoch |
| Gieren | + = nach links drehen |

Die Haltung folgt den Reglern mit begrenzter Geschwindigkeit (80 mm/s bzw. 45 °/s,
`POSE_SPEED_*` in `robot_geometry.h`), zusätzlich begrenzen die Servo-Rampen. Kann ein Bein
einen Punkt nicht erreichen oder stößt ein Gelenk an seine Kalibriergrenze, zeigt die Oberfläche
eine Warnung; das Bein behält dann seine letzte gültige Stellung.

**Erster Test:** Roboter aufbocken (Füße frei), Torque an, IK aktivieren und Höhe langsam
ändern. Alle Füße sollten sich gleichmäßig senkrecht bewegen. Bewegt sich ein Bein falsch herum,
stimmt dessen Kalibrierung (Drehrichtung) oder Montagewinkel nicht.

## Aufstehen und Hinsetzen

Beim Einschalten von Torque fahren die Beine in die **Sitzhaltung**: Körper liegt auf, Füße auf
dem Boden im gleichen Abstand wie im Stand (110 mm, `SIT_STANCE`). So drücken die Beine einen auf
dem Bauch liegenden Roboter nicht ruckartig hoch.

Karte **Laufen** → **Aufstehen** ([`posture.cpp`](../firmware/src/kinematics/posture.cpp)):

1. Körper ruckfrei auf die eingestellte Höhe heben (1,5 s, Füße bleiben stehen),
2. weicht der Fußabstand im Stand von dem im Sitzen ab, die Füße mit Schritten umsetzen – nicht
   unter Last schieben. Mit den aktuellen Werten (beide 110 mm) entfällt das.

**Hinsetzen** geht umgekehrt: Körper gerade ausrichten, ggf. Füße umsetzen, dann absenken.
Laufen ist nur im Stehen möglich. Torque aus oder Not-Aus gilt als „liegt“: Danach muss der
Roboter wieder aufstehen.

> `SIT_HEIGHT` in `robot_geometry.h` ist die Höhe der Femur-Achsen über den Fußspitzen, wenn
> der Körper flach aufliegt. Laut CAD liegt die Chassis-Unterkante 26 mm unter der Femur-Achse
> und die Fußspitze ca. 2 mm über dem Boden, also ca. 24 mm; eingestellt sind **23 mm**. Lieber
> etwas zu klein als zu groß: Zu klein, und die Füße schweben knapp über dem Boden; zu groß,
> und sie drücken den Körper beim Einschalten etwas hoch.

## Laufen

Erst **Aufstehen** (siehe oben). Tab *Steuerung* → Karte **Laufen**: linker Joystick *Fahren* (vor/zurück, seitlich), rechter
*Drehen*; am PC auch **W/S** (vor/zurück), **A/D** (seitlich), **Q/E** (drehen), **Leertaste**
(Stopp). Der Joystick schaltet den IK-Modus ein; die Körperhaltung (Höhe, Neigung, …) gilt
beim Laufen weiter.

### Wie der Gang funktioniert

[`src/kinematics/gait.cpp`](../firmware/src/kinematics/gait.cpp) führt die sechs Füße
phasengesteuert:

- **Standphase:** Der Fuß steht auf dem Boden. Im mitlaufenden Koordinatensystem wandert er genau
  entgegen der Körperbewegung (Fahren und Drehen) – er rutscht also nicht.
- **Schwungphase:** Der Fuß hebt ab, schwingt zu einem Aufsetzpunkt vor der Neutralstellung und
  setzt wieder auf. Höhe und Vorschwung folgen Profilen, deren Geschwindigkeit am Anfang und Ende
  null ist (kein Ruck, kein Aufschlagen). Der Aufsetzpunkt liegt so, dass die folgende
  Standphase symmetrisch um die Neutralstellung verläuft.
- **Anfahren/Anhalten:** Der Fahrbefehl wird über 0,5 s gerampt. Nach dem Loslassen machen die
  Beine noch einen Schritt in die Neutralstellung, dann steht der Roboter; Beine, die schon dort
  stehen, heben nicht ab.
- **Sicherheit:** Ohne neuen Fahrbefehl für 0,6 s (`WALK_TIMEOUT_MS`, z. B. WLAN weg, Handy
  gesperrt), ohne Torque oder bei Not-Aus hält der Roboter an. Die Oberfläche hält auch an,
  wenn das Fenster den Fokus verliert.

| Gangart | gleichzeitig in der Luft | Zyklus (bei 0,5 s Schrittdauer) | max. Tempo |
|---|---|---|---|
| Dreifuß (`tripod`) | 3 (vl, hl, mr ↔ ml, vr, hr) | 1,0 s | 80 mm/s |
| Ripple (`ripple`) | 2 (je Seite eins) | 1,5 s | 40 mm/s |
| Welle (`wave`) | 1 | 3,0 s | 16 mm/s |

Das Tempo ergibt sich aus dem größten Fußweg je Standphase (40 mm, `GAIT_LIMITS.stride_max`) und
der Dauer der Standphase. Fahren und Drehen gleichzeitig wird so begrenzt, dass kein Fuß
schneller als erlaubt wandert. Ein Wechsel der Gangart wirkt erst im Stillstand.

| Einstellung | Bereich | Standard | Hinweis |
|---|---|---|---|
| Tempo | 10–100 % | 60 % | skaliert den Joystick (nur in der Oberfläche) |
| Schrittdauer | 0,3–1,2 s | 0,5 s | Dauer eines Fußschwungs; kürzer = schneller, aber hektischer |
| Fußhub | 5–25 mm | 15 mm | mehr als 25 mm erreicht das Bein nicht überall |

Mit dem kurzen Femur muss das Bein zum Fußheben den Femur kräftig drehen. Bei den
Standardwerten bleiben die Gelenke im Schwung unter ca. 280 °/s, beim Tragen unter ca. 70 °/s.
Damit die Servo-Rampe die glatten Schrittbahnen nicht ausbremst, gilt im IK-Betrieb
mindestens `GAIT_SERVO_SLEW_DEG_PER_S` (450 °/s) – erst, nachdem der Übergang in die Haltung mit
der normalen Rampe abgeschlossen ist.

**Erster Test:** Roboter aufbocken (Füße frei), Torque an, Aufstehen, Tempo auf 20–30 %, Dreifuß, langsam
nach vorne. Zu sehen sein sollte: abwechselnd drei Beine heben (vorne links, hinten links, Mitte
rechts und umgekehrt), die Füße am Boden wandern gleichmäßig nach hinten. Danach auf dem Boden,
zuerst langsam.

## Tests

Die Mathematik ist hardwareunabhängig und wird auf dem PC getestet, auch in der CI bei jedem Push:

- [`kinematics_test.cpp`](../firmware/test_host/kinematics_test.cpp): IK/FK, Vorzeichen,
  Körperhaltung, Standardhaltung = Montagestellung
- [`gait_test.cpp`](../firmware/test_host/gait_test.cpp): Standbeine rutschen nicht, höchstens 3/2/1
  Beine gleichzeitig in der Luft, keine Sprünge, alle Fußpunkte erreichbar und innerhalb der
  Standardgrenzen (auch bei Extremwerten), Gelenkgeschwindigkeiten, Anhalten und Anfahren
- [`posture_test.cpp`](../firmware/test_host/posture_test.cpp): Sitzhaltung erreichbar,
  Aufstehen und Hinsetzen komplett durchgerechnet (Füße bleiben beim Heben stehen, alle Stellungen
  erreichbar und innerhalb der Grenzen)

```
cd firmware/test_host
g++ -std=c++17 -I../src -I../include kinematics_test.cpp ../src/kinematics/kinematics.cpp -o kin_test
./kin_test
g++ -std=c++17 -I../src -I../include gait_test.cpp ../src/kinematics/gait.cpp ../src/kinematics/kinematics.cpp -o gait_test
./gait_test
g++ -std=c++17 -I../src -I../include posture_test.cpp ../src/kinematics/posture.cpp ../src/kinematics/gait.cpp ../src/kinematics/kinematics.cpp -o posture_test
./posture_test
```
