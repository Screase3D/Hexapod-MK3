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
| Montagestellung (alle Servos 90°) | Coxa 0°, Femur +15°, Tibia −15° (Tibia senkrecht) |
| daraus Standardhaltung | Höhe 82,9 mm (Coxa-Achse über Boden), Fußabstand 82,5 mm |

Die Zeichnung wird von [`beingeometrie.py`](beingeometrie.py) erzeugt
(`python3 docs/beingeometrie.py`); ändern sich Maße, dort und in `robot_geometry.h` anpassen.

### Montagestellung ≠ 0°

Die Beine sind so konstruiert, dass bei Servo-Mitte (90° / 1500 µs) der Femur 15° nach oben
zeigt und die Tibia senkrecht steht – das ist ungefähr die normale Stehhaltung, die Servos haben
dort nach beiden Seiten gleich viel Weg. Die IK rechnet aber in der Konvention aus
[kalibrierung.md](kalibrierung.md) (Femur waagerecht = 0°, Tibia rechtwinklig zum Femur = 0°).
In dieser Konvention ist die Montagestellung **Coxa 0°, Femur +15°, Tibia −15°**
(`SERVO_CENTER_POSE`). Die Standardhaltung der Oberfläche (Höhe 82,9 mm, Fußabstand 82,5 mm)
entspricht genau dieser Stellung, beim Aktivieren der Körperhaltung bewegen sich die Beine also
kaum.

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
| Höhe | Abstand Coxa-Achsen ↔ Boden (50–115 mm, Standard 82,9) |
| Fußabstand | Abstand Fuß ↔ Coxa-Achse (waagerecht, 60–135 mm, Standard 82,5) |
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

## Tests

Die Mathematik ist hardwareunabhängig und wird auf dem PC getestet
([`firmware/test_host/kinematics_test.cpp`](../firmware/test_host/kinematics_test.cpp)), auch
in der CI bei jedem Push:

```
cd firmware/test_host
g++ -std=c++17 -I../src -I../include kinematics_test.cpp ../src/kinematics/kinematics.cpp -o kin_test
./kin_test
```
