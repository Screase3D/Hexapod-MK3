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
