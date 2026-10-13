# Änderungen

Die Firmware-Version steht in [`firmware/VERSION`](firmware/VERSION). Sie wird beim Build
zusammen mit dem Git-Stand eingebettet und unter *Einstellungen → Version* angezeigt (unten in der
Seitenleiste, z. B. `v0.9.0`). Für eine neue Version: `VERSION` hochzählen und hier eintragen.

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
