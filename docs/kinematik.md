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
| Montagestellung (alle Servos 90°) | Coxa 0°, Femur +30°, Tibia −30° (Tibia senkrecht) |
| daraus Standardhaltung | Höhe 72,5 mm (Coxa-Achse über Boden), Fußabstand 78,2 mm |

Die Zeichnung wird von [`beingeometrie.py`](beingeometrie.py) erzeugt
(`python3 docs/beingeometrie.py`); ändern sich Maße, dort und in `robot_geometry.h` anpassen.

### Montagestellung (Servo-Mitte)

Die Servohörner werden bei Servo-Mitte (90° / 1500 µs) in der spinnenartigen Stehhaltung
aufgesteckt: Femur 30° nach oben, Tibia senkrecht. In der Winkel-Konvention aus
[kalibrierung.md](kalibrierung.md) (Femur waagerecht = 0°, Tibia rechtwinklig zum Femur = 0°)
ist das **Coxa 0°, Femur +30°, Tibia −30°** (`ASSEMBLY_POSE`). Die Kalibrierung verwendet diese
Stellung als Bezug, und die Standardhaltung der Oberfläche (Höhe 72,5 mm, Fußabstand 78,2 mm)
entspricht ihr genau – beim Aktivieren der Körperhaltung bewegen sich die Beine also kaum.
Eine Lehre zum Ausdrucken: [montagelehre.pdf](montagelehre.pdf).

**Warum 30°?** Steiler Femur und senkrechte Tibia entlasten die Servos: Die Tibia trägt bei
senkrechter Stellung kein Drehmoment, der Femur nur über den waagerechten Abstand
`femur · cos(Femurwinkel)`. Je steiler der Femur aber schon im Stand ist, desto weniger Reserve
bleibt zum Fußheben – mit dem kurzen Femur (43 mm) und der langen Tibia (94 mm) hebt das Bein
den Fuß vor allem durch weiteres Anheben des Femurs. Überschlag (Tibia senkrecht, ca. 850 g,
3 Beine tragen, Schritt ±30 mm, Höhe ±15 mm, Fuß 25 mm heben):

| Femur im Stand | Höhe | Fußabstand | Moment Femur-Servo | Femur im Gang | Tibia im Gang | Fuß senkrecht heben bis Femur 90° |
|---|---|---|---|---|---|---|
| +15° | 82,9 mm | 82,5 mm | ≈ 1,18 kg·cm | −6…+56° | −52…+15° | 41 mm |
| **+30°** | **72,5 mm** | **78,2 mm** | **≈ 1,06 kg·cm** | **+7…+88°** | **−72…−2°** | **29 mm** |
| +45° | 63,6 mm | 71,4 mm | ≈ 0,86 kg·cm | +18…+116° | −81…−17° | 17 mm (zu wenig) |

Der MG90S schafft etwa 1,8–2,2 kg·cm; +30° ist ein Kompromiss aus Last und Hubreserve. Im Gang
kann der Fuß beim Heben zusätzlich etwas nach außen wandern, das spart Femurwinkel.

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
| Höhe | Abstand Coxa-Achsen ↔ Boden (50–115 mm, Standard 72,5) |
| Fußabstand | Abstand Fuß ↔ Coxa-Achse (waagerecht, 60–135 mm, Standard 78,2) |
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

Beim Einschalten von Torque fahren die Beine in die **Sitzhaltung**: Körper liegt auf, Füße weit
außen auf dem Boden (Fußabstand 115 mm, `SIT_STANCE`). So drücken die Beine einen auf dem Bauch
liegenden Roboter nicht ruckartig hoch.

Karte **Laufen** → **Aufstehen** ([`posture.cpp`](../firmware/src/kinematics/posture.cpp)):

1. Körper bei weit stehenden Füßen ruckfrei auf die eingestellte Höhe heben (1,5 s, Füße bleiben
   stehen),
2. Füße mit zwei Dreifuß-Schritten auf den normalen Fußabstand setzen – nicht unter Last
   schieben.

**Hinsetzen** geht umgekehrt: Körper gerade ausrichten und Füße nach außen setzen, dann absenken.
Laufen ist nur im Stehen möglich. Torque aus oder Not-Aus gilt als „liegt“: Danach muss der
Roboter wieder aufstehen.

> **Bitte nachmessen:** `SIT_HEIGHT` in `robot_geometry.h` ist die Höhe der Coxa-/Femur-Achsen
> über dem Boden, wenn der Körper flach aufliegt – derzeit geschätzt **40 mm**. Lieber etwas zu
> groß als zu klein: Zu groß, und die Füße schweben in der Sitzhaltung knapp über dem Boden;
> zu klein, und sie drücken den Körper beim Einschalten etwas hoch.

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
