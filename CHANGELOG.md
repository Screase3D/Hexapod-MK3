# Änderungen

Die Firmware-Version steht in [`firmware/VERSION`](firmware/VERSION). Sie wird beim Build
zusammen mit dem Git-Stand eingebettet und unter *Einstellungen → Version* angezeigt (unten in der
Seitenleiste, z. B. `v0.9.0`). Für eine neue Version: `VERSION` hochzählen und hier eintragen.

## 0.23.0

- **Haltungsregler wieder auf der Steuerung** (Karte *Haltung*: Höhe, Fußabstand, Verschieben, Neigen,
  Lageausgleich, Zurücksetzen); unter *Einstellungen → Bewegung* bleiben Schritt und Servos.
- **Sichtbar, was einen Regler begrenzt:** Endet ein Regler früher als seine Skala, steht darunter, warum –
  z. B. „Höhe max. 54 mm – Bein 2 Femur: Kalibriergrenze 95°“ – mit Sprung zu diesem Servo in der
  Kalibrierung („Grenze ändern“). Die Grenzen der Regler kommen allein aus den Gelenkgrenzen der Kalibrierung;
  reicht ein Servo mechanisch weiter, dort erweitern. (Telemetrie `motion.range_why`.)
- **Klettern schneller:** Dreifuß-Muster statt Ripple, Schwung doppelt statt 2,25-mal so lang, Schritte bis
  60 mm, Fußhub bis 45 mm – vorwärts 75 statt 28 mm/s; die Gelenke drehen dabei sogar langsamer als vorher.

## 0.22.0

- **Rand abschneiden ohne Vergrößern:** Der Streifen fällt einfach weg, das Bild wird entsprechend flacher
  (z. B. 640×320 statt 640×480 bei einem Drittel); der Rest bleibt in Breite und Maßstab gleich. Regler jetzt bis
  50 %. OV5640: im Sensor (auch Fotos und Videos, etwas weniger Daten); sonst blendet die Oberfläche den Streifen
  aus. Die Bildfläche nimmt das Format des Streams an (auch HD 16:9 ohne schwarze Balken).
- **WLAN-Energiesparmodus sichtbar:** *Systemübersicht* (Zeile „WLAN-Energiesparen“), *Einstellungen → WLAN* und
  die Stream-Statistik unter dem Kamerabild zeigen, ob das WLAN gerade im Modem-Sleep ist. Er ist nur an, solange
  Bluetooth läuft (Controller am Roboter); schaltet man Bluetooth aus, läuft das WLAN sofort wieder mit voller
  Leistung.

## 0.21.0

- **Kamera: Rand abschneiden** (*Kamera → Rand abschneiden*, 0–40 %, Seite unten/oben im angezeigten Bild,
  gespeichert): für Kameras, die etwas vom Roboter mitfilmen – z. B. die Oberseite der Platine, wenn die Kamera
  wegen eines kurzen Flachbandkabels weiter hinten sitzt. Der OV5640 liest den Streifen gar nicht erst aus
  (auch in Fotos und Videos); das Bild behält sein Format, links und rechts fällt je die Hälfte so viel weg.
  Drehung (auch 90°/270°) und Spiegeln werden berücksichtigt; Zoom gilt dann im verbleibenden Bild. Andere
  Sensoren: die Oberfläche blendet den Streifen aus.
- **Kamera-Symbol in der Kopfzeile**, solange ein Kamerastream läuft (auch von einem anderen Gerät; mit Anzahl
  der Zuschauer); ein Klick öffnet die Seite *Kamera*.
- Handy: leere Akku-Pille (Akku-Überwachung aus) in der Kopfzeile ausgeblendet.

## 0.20.0

- **Neue Gangart „Klettern“** (`climb`) für kleine Hindernisse wie Teppichkanten und Schwellen: Muster wie
  Ripple (immer vier Füße am Boden), aber die Füße heben fast senkrecht sehr hoch ab, schwingen oben vor und
  setzen senkrecht auf (Kastenbahn) – sie stoßen nicht gegen die Kante. Der Fußhub stellt sich selbst so hoch
  ein, wie Knie und Femur es zulassen (in der Standardhaltung 50 mm statt 15 mm); oben geht der Fuß etwas
  nach außen, damit das Knie nicht zu weit einknickt. Langsam (ca. 28 mm/s), dafür sicher.
  - Auswahl unter *Gangart*, neues Profil *Klettern* (Körper 62 mm hoch), Controller-Gangartwechsel.
  - Wechsel im Lauf von/zu Klettern: kurzer Halt, dann geht es in der neuen Gangart weiter.

## 0.19.0

- **Kamera-Zoom:** Knopf in der Kameraleiste (1× → 2× → 3× → 4× → 1×), Mausrad oder zwei Finger im Bild,
  Doppelklick zurück auf 1×; Regler auf der Seite *Kamera*, Knopf auch in der Fahrer-Ansicht.
  - *OV5640:* echter Zoom am Sensor – er liest nur einen mittigen Ausschnitt (bis 2560×1920 Pixel) und
    skaliert ihn auf die Stream-Auflösung, das Bild bekommt also mehr Details. Grenze je Auflösung:
    QVGA/VGA 4×, SVGA 3,2×, HD 2×, UXGA 1,6×. Gilt auch für Fotos. Der Zoom wird nicht gespeichert
    (nach dem Neustart 1×), damit häufiges Zoomen den Flash nicht abnutzt.
  - *Andere Sensoren:* digitaler Zoom in der Oberfläche (Bild wird vergrößert).

## 0.18.0

- **IK-Zustand wird gespeichert:** War IK an, ist es nach einem Neustart wieder an (der Roboter beginnt ohne
  Torque in der Sitzhaltung); war es aus, bleibt es aus.
- **Nie mehr schlagartig hochdrücken – egal aus welchem Zustand:**
  - *Servos einschalten:* Jeder Servo geht zuerst in die sichere Sitzhaltung und fährt dann mit höchstens 45 °/s
    zu seinem Sollwert. Vorher sprang er mit voller Geschwindigkeit auf den letzten Sollwert – stand der z. B. noch
    auf Montagestellung (= Standhaltung), drückte sich ein liegender Roboter schlagartig hoch.
  - *„Alle auf 1500 µs / Montagestellung / gleicher Winkel“* fahren ebenfalls mit höchstens 45 °/s (vorher 240 °/s).
  - IK einschalten, Aufstehen und Hinsetzen beginnen in der tatsächlichen Haltung (seit 0.17.1); Not-Aus,
    Akku- und Update-Sperre schalten Torque ganz ab, danach gilt wieder das sichere Einschalten.

## 0.17.1

- **Behoben: Sprung beim Einschalten von IK.** Nach dem Start sitzt der Roboter (Servos in Sitzhaltung,
  IK aus). Schaltete man dann *IK* ein, fuhr er ohne Aufsteh-Ablauf mit voller Servogeschwindigkeit in die
  Standhaltung – belastend für die Servos. Ebenso sprang *Hinsetzen* aus dem manuellen Modus erst hoch.
  Jetzt schätzt die Firmware beim Einschalten die tatsächliche Haltung aus den Servowinkeln: Sitzt er, bleibt
  er sitzen (Aufstehen per Knopf); steht er schon, geht es auf seiner Höhe weiter. Bis die IK-Haltung erreicht
  ist, fahren die Servos höchstens 90 °/s.

## 0.17.0

- **Zappeln reagiert sofort:** Das Anheben wurde bisher erst nach Sekunden erkannt (Schwellen zu hoch, und während
  „Lebendig“-Bewegungen gar nicht). Jetzt drei Hinweise – Beschleunigung, Drehung und Neigung gegenüber der
  gewollten Körperlage –, empfindlichere Schwellen, auch während eigener Bewegungen; im PC-Test erkennt er sanftes
  Anheben nach 40 ms. **Empfindlichkeit** einstellbar (gering/mittel/hoch).
- **Gähnen, Seufzen, Strecken deutlich ausdrucksstärker:** Kopf weit hoch, Körper hebt und senkt sich sichtbar, beim
  Strecken Katzenbuckel und beide Vorderbeine nacheinander nach vorne gestreckt.
- **Oberfläche neu geordnet:**
  - *Steuerung* nur noch mit dem, was man zum Fahren braucht: Kamera, 3D-Ansicht, Bewegung (Gangart, Tempo), kompakte
    Karte Haltung (Servos, IK, Profile). Der Zustand zeigt jetzt auch „schläft“ und „zappelt“.
  - *Spielereien* heißt jetzt **Verhalten** und enthält alles zu Verhalten und Emotionen: Emotes, Spiele, Augen,
    Choreografien und die neue Karte **Lebendigkeit** (Lebendig, Atmen, Langeweile, Zappeln, Laufen mit Charakter,
    Augen schauen mit, Stimmung).
  - Neue Seite **Kamera & Lagesensor**: Live-Vorschau, alle Kameraeinstellungen, Lage-Anzeige, Lagesensor einrichten.
  - *Einstellungen* gegliedert in **Bewegung** (Haltung mit allen Reglern und Lageausgleich, Laufen, Servos),
    **Verbindung** (WLAN, Signal messen) und **System** (Akku, Version, Firmware-Update, Oberfläche).
  - Alte Adresse `#spielereien` führt weiter auf *Verhalten*.

## 0.16.0

**Lebendig wirken** – alles einzeln schaltbar unter *Einstellungen → Lebendig wirken* (Standard: an):

- **Atmen:** Im Stand hebt und senkt sich der Körper ganz leicht (±2 mm alle 4 s), die Augen atmen im Takt mit.
- **Langeweile:** Steht er lange untätig, wird er gelangweilt – gähnt, seufzt, schaut sich um, Augen gedimmt –
  und legt sich nach der Einschlafzeit (2–30 min, Standard 5) schlafen: setzt sich hin, Augen schlafen blau.
  Fahren, Körper-Joystick, Gesten oder Aufstehen wecken ihn: er steht auf und streckt sich.
- **Zappeln beim Hochheben:** Der Lagesensor erkennt das Anheben; er strampelt dann mit den Beinen, Augen orange
  blitzend. Immer nur drei Beine in der Luft – bei einer Fehlerkennung auf dem Boden steht er trotzdem sicher.
- **Laufen mit Charakter:** lehnt sich beim Anfahren zurück, schwingt beim Anhalten nach, schaut in die Kurve und
  beim Seitwärtslaufen in die Laufrichtung, Hüftschwung im Schritttakt; nach längerem Laufen schaut er sich
  beim Anhalten manchmal um.
- **Augen schauen mit:** Beim Drehen, Seitwärtslaufen und Umschauen leuchtet das Auge auf der Seite heller.
- Firmware: neuer Baustein `life` mit PC-Tests (auch in der CI), Befehl `life`, Telemetrie `motion.life`;
  „Lebendig“ kennt jetzt Gähnen, Seufzen und Strecken.

## 0.15.0

- **Mehr Emotes, jetzt mit Augen:** neu Neugierig, Sauer (rot flackernd, stampft), Angriff (duckt sich, stößt
  zweimal vor, rot blitzend), Überrascht, Traurig, Müde, Verliebt (Herzschlag in Pink) und Party; die bisherigen
  zeigen jetzt ebenfalls passende Augen. „Überrasch mich“ spielt ein zufälliges Emote.
- **Spiele:** *Orakel* (denkt nach, antwortet mit Kopf und Augen) und *Würfeln* (schüttelt sich, nickt die Zahl).
- **Augen frei wählen:** Karte mit Farbfeldern (Wut, Ruhig, Herz, Blitz, Party …), Farbe, Effekt und Dauer,
  animierte Vorschau bzw. Anzeige, was der Roboter gerade zeigt. Neue Effekte *Flackern*, *Blitzen*,
  *Hin und her* und *Müde* – auch für die Zustände auf der Seite LEDs. Choreografien haben einen Schritt „Augen“.
- **Tempo** für Choreografien und Emotes (50–200 %).
- **Controller in der Kopfzeile:** Symbol mit Akkustand, solange ein Controller verbunden ist (Bluetooth oder
  Gamepad im Browser), Tipp darauf öffnet die Controller-Seite.
- Firmware: Befehl `eyes` (Augen kurz vorgeben), Telemetrie `eyes`.

## 0.14.0

- **Neue Seite „Spielereien“:**
  - **Emotes** auf einen Tipp: Hallo (Vorderbeine heben und winken), Verbeugen, Freuen, Umschauen,
    Ängstlich, Strecken sowie die Gesten Kreisen, Ja, Nein, Liegestütz und Tanzen. Sitzt der Roboter, steht er
    vorher auf.
  - **Choreografien:** Abfolgen aus Emotes, Gesten, Körperbewegungen (verschieben/neigen/drehen), Höhe,
    Laufen, Vorderbeinen, Winken und Pausen, zusammengestellt im Editor (Schritte hinzufügen, verschieben,
    entfernen, Endlosschleife, Entwurf direkt abspielen). Vorlagen: Disco, Begrüßung, Wache schieben – als
    Kopie bearbeitbar. Eigene Choreografien liegen im Browser, Export/Import als JSON.
  - Leertaste, zweiter Tipp, Stopp in der Leiste, Seitenwechsel oder Verbindungsabbruch halten an; danach
    geht der Körper in die Haltung zurück, die Vorderbeine werden abgesetzt und die Höhe wiederhergestellt.
    Höhenschritte bleiben im erreichbaren Bereich (`motion.range`).

## 0.13.0

- **Haltungs-Regler passen sich an:** Unter jedem Regler (Höhe, Fußabstand, Verschieben, Neigen, Gieren)
  zeigt ein Band, wie weit die Beine bei den übrigen Einstellungen noch reichen – stellt man z. B. den
  Körper tief, schrumpfen die Bänder für Verschieben und Neigen. Der Regler lässt sich nur bis zum Rand des
  Bandes ziehen, am Rand wird der Wert gelb. Die Grenzen rechnet die Firmware mit der echten Kinematik und
  den Gelenkgrenzen der Kalibrierung (`motion.range` in der Telemetrie).

## 0.12.2

Weitere Fehlerbehebungen aus der Durchsicht:

- **Aufstehen:** Wurde die Standhöhe oder der Fußabstand während des Aufstehens geändert (Regler, Profil,
  Controller), sprang der Körper – im ungünstigsten Fall um 65 mm in einem Regelschritt. Das Aufstehen
  läuft jetzt mit den Werten vom Beginn zu Ende, danach gleitet die Haltung zum neuen Wert.
- **Kalibrierung im Stehen:** Drehrichtung umkehren, Standardwerte, gespeicherte Werte laden oder eine
  Kalibrierung importieren wirkte im IK-Betrieb sofort auf alle Gelenke – sie sprangen. Diese Befehle
  schalten jetzt zuerst in den manuellen Modus (Servos bleiben stehen).
- **Firmware-Update über PlatformIO:** Die Servos wurden zwar abgeschaltet, ließen sich aber während des
  Uploads wieder einschalten; jetzt gesperrt wie beim Upload über die Weboberfläche. Die Sperre bei leerem
  Akku und die beim Update kommen sich nicht mehr in die Quere.
- **WLAN:** Riss die Verbindung zum Heimnetz ab, während der eigene Zugangspunkt noch kurz offen war, wurde
  er trotzdem abgeschaltet – der Roboter war dann gar nicht mehr erreichbar, bis das Heimnetz wiederkam.
  Jetzt bleibt der Zugangspunkt offen und das Heimnetz wird nach 10 s neu versucht (wie nach einem
  gescheiterten Verbinden).
- **Webserver:** Der Gegendruck bei schwachem WLAN (0.11.0) griff nur für Clients, die schon einmal einen
  Befehl geschickt hatten; jetzt für alle ab dem Verbinden. Clients, die 10 s lang gar nichts mehr
  annehmen (tote Verbindung), werden getrennt statt weiter Speicher zu belegen. Die Sendefrist zählt ab dem
  letzten Fortschritt, damit die Oberfläche auch bei sehr schwachem WLAN vollständig lädt.
- Doku: Telemetrie `ctl` und `cam.bt_fs`, Befehl `client_log`.

## 0.12.1

Fehlerbehebungen nach einer gründlichen Durchsicht des ganzen Codes:

- **Laufen:** Musste ein Bein am Rand seiner Reichweite auf ein Nachbarbein in der Luft warten, lief der
  Körper ungebremst weiter – nach schnellen Richtungswechseln knickte das Knie dann bis auf 9° ein oder ein
  Fuß kam ganz aus der Reichweite. Jetzt bremst der Körper rechtzeitig (30 000 zufällige Richtungswechsel im
  Test: Knie immer ≥ 25°). Ändern sich Richtung, Schrittdauer oder Fußhub im Lauf, gleitet das Tempo auf den
  neuen Höchstwert, statt in einem Regelschritt gekappt zu werden (vorher bis 4800 mm/s², jetzt unter 800).
- **„Lebendig“ und Laufen:** Hob „Lebendig“ gerade einen Fuß, als ein Fahrbefehl kam, waren kurz vier Beine
  in der Luft. Schritte beginnen jetzt erst, wenn „Lebendig“ ausgeblendet ist (0,4 s).
- **Fahrbefehl im Handbetrieb** (nach dem Start, Servos in Sitzstellung) ließ den Roboter ohne
  Aufstehen-Ablauf direkt in die Standhaltung springen – wird jetzt ignoriert, erst „Aufstehen“.
- **Ungültige Zahlen** (z. B. „unendlich“ vom Browser-Gamepad) konnten über NaN die Servos über ihre
  Anschläge fahren – werden jetzt an allen Stellen abgefangen.
- „Alle auf 1500 µs“ bzw. „Montagestellung“ direkt nach dem IK-Betrieb konnte vom letzten Regelschritt
  überschrieben werden.
- **Webserver:** Ein abgebrochener Upload (Kalibrierung importieren, Handy verlässt das WLAN) blockierte den
  Server dauerhaft – auch der Not-Aus über das Netz ging dann nicht mehr. Ein fehlerhafter
  `servo_us`-Befehl schaltete den IK-Betrieb ab.
- **Controller:** „Vergessen“ bei ausgeschaltetem Bluetooth ging verloren (er verband sich später wieder);
  der Controller konnte ein längeres Funk-Timeout aushandeln (Stopp nach Abbruch erst nach 6 s statt 2 s);
  beim Verbinden gehaltene Tasten lösten aus, gehaltene Steuerkreuz-Tasten wiederholten sofort; das mit dem
  Steuerkreuz gewählte Tempo wurde bei jedem Speichern der Controller-Einstellungen zurückgesetzt;
  Browser-Gamepad mit Bluetooth an (WLAN im Energiesparmodus) wurde ständig als getrennt erkannt.
- **Oberfläche:** Eine hängende Verbindung wurde zwar erkannt, aber erst nach langem Warten neu aufgebaut –
  so lange liefen Befehle ins Leere, auch STOP (jetzt sofort neu verbinden, Not-Aus über die Notfall-Adresse).
  Esc im Dialog „Taste belegen“ löste zusätzlich den Not-Aus aus. Pfeiltasten am Testregler der
  Kalibrierung verstellten den Rohpuls an den Gelenkgrenzen vorbei. Schalter, Import und Profil-Löschen per
  Tastatur erreichbar; Löschen eines eigenen Profils als eigener Knopf (ein knapp verfehltes × hat das Profil
  angewendet). Beinmaße werden nachgeladen, wenn der erste Versuch scheitert. Die 3D-Ansicht holt keine
  Live-Daten, solange die Fahrer-Ansicht darüber liegt. Leertaste auf einem per Tab gewählten Knopf löst ihn
  aus (sonst weiter Stopp). Am Handy zeigt der Akku nur Prozent, damit der Seitentitel lesbar bleibt.

## 0.12.0

- **Verbindung bei schwachem WLAN robuster:** Kam das WLAN nicht nach (voller Sendepuffer), wartete der
  Webserver bis zu 4 s beim Senden der Telemetrie – in der Zeit blieben auch eingehende Befehle liegen, der
  Roboter konnte beim Laufen kurz anhalten. Mit den neuen 20-Hz-Daten der 3D-Ansicht wäre das häufiger
  passiert. Jetzt lässt er regelmäßige Nachrichten (Telemetrie, Live-Daten) für einen Client aus, solange
  dessen Puffer voll ist, und stellt nicht mehr als zwei davon an. Wie oft das in der letzten Minute
  vorkam, steht im Tooltip der Verbindungsanzeige (Telemetrie `ws_drop`).
- **Signal messen** (Einstellungen): Empfangsstärke live und als 10-s-Mittel mit Bezeichnung, mehrere
  Messungen untereinander mit dem Unterschied zur ersten (z. B. „ohne Antenne“, „Antenne oben“). Bleibt nach
  einem Neustart erhalten – praktisch zum Prüfen der externen Antenne.

## 0.11.1

- 3D-Ansicht: Die Tibia hat jetzt die gebogene Form des Bauteils aus dem CAD (vom Knie erst nach außen,
  dann der Knick nach unten zur Fußspitze) statt eines geraden Stabs. Für die Kinematik zählt weiterhin
  nur die gerade Linie Knie → Fußspitze.

## 0.11.0

- **3D-Ansicht** auf der Steuerungsseite: Der Roboter als Live-Modell, gerechnet aus den aktuellen
  Gelenkwinkeln (Vorwärtskinematik) und der Körperlage – mit Schatten, Beinen in der Luft (farbig, mit
  Höhe über dem Boden), der Standfläche der Füße am Boden und dem Lot des Schwerpunkts (grün = sicher,
  gelb/rot = am bzw. außerhalb des Rands) samt Standreserve in mm. Beim Laufen wandert der Boden mit.
  Ansichten 3D, Oben, Seite, Vorne; Ziehen dreht, Strg + Mausrad zoomt, Doppelklick setzt zurück. Die
  Daten kommen mit 20 Hz nur, solange die Ansicht sichtbar ist (`{"cmd":"live"}`), sonst aus der
  Telemetrie; auch im Handbetrieb (Kalibrierung) zu sehen.
- **Weichere Körperbewegungen:** Haltung, Körper-Joystick, Profile und Lageausgleich folgen jetzt mit
  begrenzter Beschleunigung – sanft anfahren, rechtzeitig abbremsen, ohne Überschwingen (vorher mit
  voller Geschwindigkeit los und abrupt gestoppt).

## 0.10.0

- **Gangart im Lauf wechseln:** Dreifuß, Ripple und Welle lassen sich jetzt jederzeit umschalten (Oberfläche,
  Profil oder Controller), ohne anzuhalten. Zu einer langsameren Gangart bremst der Roboter gleitend ab,
  dann übernimmt jedes Bein die Phase, die zu seiner Fußstellung passt; laufende Schritte werden in ihrem
  Tempo zu Ende geführt, kein Fuß springt. In wenigen Schritten (ca. 2–7 s) finden die Beine in den neuen
  Takt, nie zwei Nachbarbeine gleichzeitig in der Luft; währenddessen 75 % Tempo, danach gleitend volles
  Tempo. Unter den Joysticks steht „Gangart wechselt …“, solange der Übergang läuft.
- **Weichere, sicherere Schritte bei wilden Joystick-Bewegungen:** Würde ein Fuß vor seinem Schritt den
  erlaubten Bereich verlassen (schnelle Richtungsumkehr, Anlaufen in Kurven), setzt er vorher um. Hebt ein Fuß
  nah am Körper ab, führt die Schwungbahn ihn erst etwas nach außen. Im Zufalls-Dauertest bleibt das Knie
  so immer über 25° (vorher bis 5°, mit kurzen Fehlstellungen); die größten Fußsprünge sind kleiner.
- Schwungbahnen folgen einem sich verschiebenden Aufsetzpunkt (Tempoänderung im Schritt) ohne Sprung.
- Gangsteuerung mit gemeinsamem Takt und Bein-Versätzen: kein Auseinanderdriften der Beine durch Rundung.

## 0.9.0

- **Besseres Kamerabild:** Bis jetzt lief der Sensor mit Werkseinstellungen. Neu unter ⚙ an der Kamera:
  Vorgaben *Automatisch*, *Bewegung* (kürzere Belichtung – weniger Unschärfe beim Laufen), *Innen*,
  *Draußen*, *Nacht* sowie Regler für Helligkeit, Belichtung, maximale Verstärkung, Kontrast, Sättigung,
  Schärfe (OV5640), Weißabgleich, Nachtmodus, Linsen- und Pixelfehler-Korrektur. Wird gespeichert und wirkt
  sofort; die Auflösung wird dabei nur noch bei echten Änderungen neu gesetzt (kein kurzes Haken).
- **Video aufnehmen:** Knopf ● neben dem Foto – nimmt den Stream im Browser auf und speichert ihn als Datei
  (WebM, auf dem iPhone MP4). Kostet den ESP nichts zusätzlich; Stream stoppen beendet die Aufnahme.
- **Verlauf** in der Systemübersicht: Akkuspannung, WLAN-Signal, Chip-Temperatur und freier Speicher der
  letzten 30 min (alle 10 s), mit Fadenkreuz/Tooltip und Tabelle – z. B. um Akkuverbrauch oder
  WLAN-Aussetzer beim Laufen zu sehen.

## 0.8.2

- **WLAN beim Start zuverlässig und schnell:** Lehnte der Router die erste Anmeldung ab (typisch direkt nach
  einem Neustart: „Anmeldung abgelaufen“ / Handshake-Timeout – er kennt noch die alte Sitzung), hielt die
  Firmware das für ein falsches Passwort, startete den Access Point und versuchte es erst nach 60 s wieder.
  Jetzt: mehrere Anläufe innerhalb von 20 s, „Passwort falsch?“ erst nach drei Ablehnungen in Folge;
  im Access-Point-Modus neue Versuche nach 10, 20, 40 s, dann jede Minute (mit Geräten am AP alle 3 min);
  bei Repeatern/Mesh der stärkste Zugangspunkt.
- Ereignisprotokoll: „WLAN verbunden … nach 4,2 s, 2 Anläufe“, beim Start der Grund, falls das Heimnetz
  nicht erreicht wurde. *Einstellungen → WLAN* zeigt Signalqualität, Kanal, Anläufe und den nächsten Versuch.
- **Kamera mit Bluetooth:** Solange Bluetooth an ist, streamt sie höchstens in CIF (400×296) – statt 4–5
  kommen so deutlich mehr Bilder pro Sekunde an. Einstellbar unter ⚙ an der Kamera (*Mit Bluetooth*);
  die Statistik unter dem Bild zeigt, wenn die kleinere Auflösung aktiv ist.
- Warnung im Ereignisprotokoll, wenn der Controller-Akku unter 15 % fällt.
- *Kopieren* im Ereignisprotokoll klappt jetzt auch am Handy (ohne HTTPS) und setzt Firmware-Version und
  Laufzeit an den Anfang. WLAN-Signal im Tooltip der Verbindungsanzeige.

## 0.8.1

- **Controller-Seite neu:** Der Controller ist groß abgebildet, an jeder Taste steht ihre Funktion. Antippen
  (Taste oder Beschriftung) öffnet eine Auswahl nach Themen, die auch zeigt, wo eine Funktion schon liegt.
  Gedrückte Tasten, Sticks und Trigger bewegen sich live mit; Sticks und Trigger je Modus umschaltbar.
  Auf dem Handy: Grafik plus Liste zum Antippen.
- **Sticks tauschen** (z. B. für Linkshänder): rechter Stick fährt, linker dreht – in allen Modi.

## 0.8.0

- **Schneller laufen:** Schritte bis 70 statt 50 mm (Coxa ±20° statt ±14°). Dreifuß in der
  Standardhaltung 175 statt 125 mm/s, Profil *Sport* (0,3 s, 15 mm Hub) 216 mm/s, Ripple 88 statt
  62 mm/s, Welle 35 statt 25 mm/s. Bei hohem Fußhub bleibt der Schritt kürzer (25 mm Hub: 50 mm), damit
  kein Gelenk im Schwung schneller als ca. 550 °/s drehen muss (MG90S: ca. 600 °/s ohne Last).
- **Lageausgleich viermal so schnell:** 10° Schräglage sind nach ca. 0,7 s statt 3 s ausgeglichen, ohne
  Überschwingen (Korrektur höchstens 30 °/s). Beim Laufen bewusst ruhiger (reagiert nicht auf jeden Schritt).
- „WebSocket … fehlgeschlagen (errno 0)“ erscheint nicht mehr – eine schon geschlossene Verbindung (z. B.
  ein alter Tab) ist kein Fehler.

## 0.7.4

- **Behoben: Verbindung der Oberfläche brach mit verbundenem Controller im Sekundentakt ab.** Ursache war
  ein Programmfehler, nicht der Funk: Der Name des Controllers kam als Datenmüll in die Telemetrie
  (ArduinoJson 7 speichert ein `const char[]` nur als Zeiger – auf eine Kopie, die beim Senden schon
  nicht mehr existierte). Den ungültigen Text verwirft der Browser mitsamt der Verbindung – bei jeder
  Telemetrie (4× pro Sekunde) aufs Neue.
- Gekürzte Texte (Meldungen, Ereignisprotokoll) enden nie mitten in einem Umlaut.
- Sendefehler, weil der Browser die Verbindung schon geschlossen hat (Seite neu geladen, Handy
  gesperrt), sind keine Warnung mehr; echte Fehler mit Fehlernummer.

## 0.7.3

- **„Keine Verbindung“ im Sekundentakt bei eingeschaltetem Bluetooth:** Stockt das WLAN (Bluetooth teilt
  sich die Antenne), schickte der Webserver WebSocket-Nachrichten unter Umständen nur halb – der Browser
  verwarf die Verbindung dann. Jetzt wird jede Nachricht ganz oder gar nicht gesendet; hängt eine
  Verbindung, wird sie sauber geschlossen und protokolliert.
- Die Oberfläche verbindet nach einem Abbruch sofort neu und zeigt „Keine Verbindung“ erst nach 2,5 s
  (vorher „Verbinde …“). Den Grund jedes Abbruchs schreibt sie ins Ereignisprotokoll des Roboters.
- Bluetooth: wieder 15–30 ms Intervall (mit 30–50 ms riss die Verbindung beim Koppeln ständig ab,
  Code 520 = Funk-Timeout), Timeout 2 s statt 1 s, Verbindungsaufbau mit halber Funkzeit, Pausen nach
  Fehlversuchen (1 → 8 s), Sendeleistung fest +9 dBm. Signalstärke des Controllers im Protokoll,
  Fehlercodes mit Klartext.

## 0.7.2

- **Bluetooth-Controller und WLAN gleichzeitig:** Der Controller wünscht sich nach dem Verbinden
  ein Intervall von wenigen Millisekunden; damit belegt Bluetooth die gemeinsame Antenne so oft, dass
  das WLAN kaum noch durchkommt. Der Roboter lehnt jetzt alles unter 30 ms ab (verbindet mit 30–50 ms,
  so wie ein iPhone). Trennt der Controller daraufhin, bekommt er sein Intervall doch (steht im Protokoll).
- Koppeln zuverlässiger: Startet der Controller die Verschlüsselung selbst, wartet der Roboter darauf,
  statt abzubrechen; LE Secure Connections erlaubt.
- Vibration geht an den Ausgabebericht des Controllers (vorher evtl. an ein falsches Merkmal).
- Ereignisprotokoll: Intervall, Speicher und Verschlüsselung beim Verbinden, Wunsch des Controllers
  nach anderem Intervall, Grund einer Bluetooth-Trennung; längere Einträge.

## 0.7.1

- **Fehler behoben:** Mit eingeschaltetem *Bluetooth am Roboter* brach das WLAN im Sekundentakt ab und der
  Controller ließ sich nicht koppeln. Das WLAN lief ohne Energiesparmodus; WLAN und Bluetooth teilen sich
  aber die Antenne und brauchen dafür den Modem-Sleep. Er wird jetzt nur eingeschaltet, solange Bluetooth
  an ist (sonst bleibt die Latenz minimal). Suche und Verbindung belegen außerdem weniger Funkzeit.
- Ereignisprotokoll: Grund bei WLAN-Trennungen; beim Koppeln, ob überhaupt Bluetooth-Geräte bzw. ein
  Xbox-Controller gesehen wurden.

## 0.7.0

- **Fahrer-Ansicht**: Kamera im Vollbild mit Joysticks darüber (Modus, Aufstehen/Hinsetzen,
  Vorderbeine/Winken, Gesten, HUD, STOP) – am Handy quer wie eine Fernsteuerung.
- **Profile** in der Karte Körperhaltung: Normal, Stabil (tief, breit, Ripple), Gelände (hoch,
  hoher Fußhub), Sport (schnell), Schleichen (Welle) sowie bis zu vier eigene (im Browser gespeichert).
- **Firmware-Update über WLAN**: Upload in den Einstellungen oder `pio run -e hexapod_ota -t upload`.
- **Ereignisprotokoll** (Systemübersicht): Neustartgrund (erkennt Unterspannung/Absturz), WLAN,
  Not-Aus, Servos, Akku, Lagesensor, Controller, Kalibrierung, Updates; Punkt im Menü bei Warnungen.
- Antwortzeit der Verbindung in der Seitenleiste, Warnung bei langsamer Verbindung.
- Als App auf den Home-Bildschirm (Icon, ohne Browserleisten auf iOS).
- Kurzes Vibrieren beim Greifen eines Joysticks (Android), Einstellungen übersichtlicher angeordnet.

## 0.6.0

- **Version** in den Einstellungen und in der Seitenleiste (Firmware, Git-Stand, Build-Datum).
  Stammt eine offene Seite von einer älteren Firmware, meldet die Oberfläche das.
- **LED-Einstellungen** (neue Seite *LEDs*): Farbe und Effekt der Augen je Zustand (Servos aus,
  steht, läuft, Vorderbeine, Geste, …), eine Farbe für alles oder aus; Helligkeit; Board-LED.
- **Xbox-Controller** (neue Seite *Controller*): direkt per Bluetooth (BLE) am Roboter oder
  am Handy/PC über den Browser; Belegung der Tasten, Totzone, Kennlinie und Tempo einstellbar.

## Davor

Bis hierher meldete die Firmware durchgehend „0.1.0“. Die wichtigsten Stände in umgekehrter
Reihenfolge:

- Karte *Bewegung* mit drei Modi: Laufen, Körper (verschieben/neigen, Gesten), Vorderbeine.
  Vorderbeine heben und steuern (Schwerpunkt nach hinten, IK-geprüfter Arbeitsraum), Winken.
  Gesten: Kreisen, Ja, Nein, Liegestütz, Tanzen. „Lebendig“: kleine zufällige Bewegungen im Stand.
- Unerreichbare Haltungen werden begrenzt und gemeldet; Hinweis, wenn die Haltung das Lauftempo
  einschränkt. Langsames Laufen mit längeren statt kürzeren Schritten. Board-LED vor der Kamera nur
  noch beim Start.
- Lagesensor: Einbaulage, Nullpunkt und Gyro kalibrierbar, Lageausgleich. Haltung, Gang und
  Stellgeschwindigkeit werden gespeichert. Schnelleres Laufen mit richtungsabhängiger
  Reichweitengrenze. Neue Oberfläche mit Seitenleiste.
- Gangarten Dreifuß, Ripple und Welle mit weichen Fußbahnen; Joysticks und Tastatur; Aufstehen und
  Hinsetzen.
- Inverse Kinematik und Körperhaltung (Höhe, Fußabstand, Verschieben, Neigen).
- Grundgerüst: WLAN, Weboberfläche, Kamerastream, Servo-Kalibrierung, Akku- und
  Sicherheitsfunktionen.
