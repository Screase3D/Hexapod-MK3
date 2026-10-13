# Änderungen

Die Firmware-Version steht in [`firmware/VERSION`](firmware/VERSION). Sie wird beim Build
zusammen mit dem Git-Stand eingebettet und unter *Einstellungen → Version* angezeigt (unten in der
Seitenleiste, z. B. `v0.7.0`). Für eine neue Version: `VERSION` hochzählen und hier eintragen.

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
