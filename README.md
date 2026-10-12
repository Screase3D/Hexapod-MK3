# Hexapod-MK3

Dritte Version des 3D-gedruckten Hexapods – diesmal mit ESP32-S3-CAM.
18× MG90S an 2× PCA9685, MPU6050, Kamera, Steuerung per Browser über WLAN.

**Ziel:** natürliche, fließende Bewegungen mit inverser Kinematik, gesteuert über eine
Weboberfläche, die der ESP32 selbst ausliefert (PC und Smartphone), inklusive Kamerastream –
später erweitert um KI-Funktionen mit der Kamera.

## Stand

| Phase | Inhalt | Status |
|---|---|---|
| 1 | Grundgerüst: WLAN, Webserver, Kamerastream (mit Drehung), Servotreiber, **Servo-Kalibrierung**, manuelle Servosteuerung, I²C-Scan, Akku-/Sicherheitsfunktionen, CI-Build | ✅ läuft auf der Hardware |
| 2 | Kinematik: inverse Kinematik je Bein, Körperhaltung (Höhe, Fußabstand, Verschieben, Rollen/Nicken/Gieren) | ✅ läuft am Roboter, Beinmaße aus dem CAD ([Zeichnung](docs/beingeometrie.png), [docs/kinematik.md](docs/kinematik.md)), inklusive Aufstehen/Hinsetzen; Haltung wird gespeichert |
| 3 | Gangarten (Dreifuß, Ripple, Welle) mit weichen Fußbahnen, Touch-Joysticks und Tastatur in der UI, Verbindungs-Watchdog | ✅ läuft am Roboter; jetzt mit richtungsabhängiger Reichweitengrenze, bis 125 mm/s ([docs/kinematik.md](docs/kinematik.md#laufen)) – schnellere Fassung noch nicht am Roboter getestet |
| 4 | Lagesensor-Kalibrierung (Einbaulage, Nullpunkt, Gyro) und **Lageausgleich** (Körper waagerecht halten); Geländeanpassung | 🟡 Lageausgleich implementiert und auf dem PC getestet – noch nicht am Roboter; Geländeanpassung geplant |
| 5 | Kamera/KI: Objekterkennung (auf dem ESP32-S3 oder am PC über den Stream), Folgen, OTA-Updates | Ideen |

## Repository

```
docs/
  hexapod_schaltplan_v4.png          Verdrahtungsplan (PCA #1 = rechte Beine, #2 = linke)
  ESP32_S3_Camera_Instructions.pdf   Board-Doku (Pinout S. 4)
  hardware.md                        Pinbelegung, Servo-Zuordnung, Hinweise zur Schaltung
  kalibrierung.md                    Gelenk-Konvention und Kalibrierablauf
  kinematik.md                       Beinmaße, Koordinatensysteme, Körperhaltung
  beingeometrie.svg/.png             Maßzeichnung Bein + Körper (erzeugt von beingeometrie.py)
  montagelehre.pdf                   Lehre 1:1 zum Aufstecken der Servohörner (montagelehre.py)
  api.md                             REST-, WebSocket- und Stream-Schnittstellen
firmware/                            PlatformIO-Projekt
  platformio.ini
  include/config.h                   alle Pins, Adressen, Grenzwerte
  include/robot_geometry.h           Beinmaße und Montagepunkte (für die IK)
  src/main.cpp                       Start, Regel-Task, Sicherheitslogik
  src/servo/                         PCA9685-Treiber, Servo-Mapping, Kalibrierung, ServoController
  src/kinematics/                    inverse/Vorwärts-Kinematik, Gangarten (reine Mathematik)
  src/motion/                        Körperhaltung + Gang → IK → Servos (IK-Modus)
  src/sensors/                       Akku, MPU6050, I²C-Scan
  src/net/                           WLAN, HTTP/WebSocket, Kamera, Befehle
  src/ui/                            Status-LEDs (Augen)
  web/                               Weboberfläche (HTML/CSS/JS, wird in die Firmware eingebettet)
  tools/mock_server.py               Oberfläche am PC ohne Roboter testen
  test_host/                         Tests, die auf dem PC laufen (Kinematik, Gangarten)
.github/workflows/firmware.yml       CI: Tests + Firmware-Build bei jedem Push
```

## Kompilieren und Flashen

### Einmalig einrichten

1. **[Git](https://git-scm.com/download/win)** installieren – die ESP32-Plattform braucht es
   auch dann, wenn der Code als ZIP geladen wird (sonst: „Git not found in PATH“). Im
   Installer die Standardoption „Git from the command line and also from 3rd-party software“
   beibehalten. Alternativ in PowerShell: `winget install --id Git.Git -e`.
2. **Lange Dateipfade erlauben** (nur Windows). Die ESP32-Bibliotheken enthalten Dateien mit
   sehr tiefen Pfaden; ohne diese Einstellung bricht der erste Build beim Entpacken mit
   `FileNotFoundError … esp32-arduino-libs …` ab. PowerShell **als Administrator** öffnen und
   ausführen, danach Windows neu starten:
   ```
   New-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" -Name "LongPathsEnabled" -Value 1 -PropertyType DWORD -Force
   git config --global core.longpaths true
   ```
3. **[VS Code](https://code.visualstudio.com/)** installieren, darin die Erweiterung
   **PlatformIO IDE** (Erweiterungen-Ansicht, nach „PlatformIO“ suchen). Danach VS Code
   komplett schließen und neu starten, damit Git im PATH gefunden wird.
4. **Code holen** – am besten in einen kurzen Pfad ohne Leerzeichen, z. B. `C:\dev\` (Windows
   hat Probleme mit sehr langen Pfaden):
   ```
   git clone https://github.com/Screase3D/Hexapod-MK3.git
   ```
   Ohne Git: auf GitHub *Code* → *Download ZIP* und entpacken.
5. In VS Code **Datei → Ordner öffnen → `Hexapod-MK3/firmware`** wählen (der Ordner mit der
   `platformio.ini`, nicht das Repo-Hauptverzeichnis).
6. WLAN-Zugangsdaten müssen **nicht** in den Code: Das Heimnetz wird nach dem Flashen über die
   Oberfläche eingerichtet (siehe [Erster Start](#erster-start)). Nur wer sie trotzdem fest
   einkompilieren will, kopiert `include/secrets.example.h` nach `include/secrets.h`.

### Kompilieren

- In der blauen Statusleiste unten auf **✓ (Build)** klicken – oder im Terminal `pio run`.
- Beim **ersten** Build lädt PlatformIO die Plattform
  [pioarduino](https://github.com/pioarduino/platform-espressif32) (Arduino-Core 3.3 auf
  ESP-IDF 5.5) samt Compiler. Das dauert je nach Internet 5–20 Minuten und belegt rund
  **4 GB** Platz. Danach dauert ein Build unter einer Minute.
- Bricht der erste Build beim Entpacken ab, vor dem nächsten Versuch den Ordner
  `%USERPROFILE%\.platformio\.cache\tmp` leeren und – falls vorhanden – den halb entpackten
  Ordner `%USERPROFILE%\.platformio\packages\framework-arduinoespressif32-libs` löschen.
- Erfolgreich, wenn am Ende `[SUCCESS]` steht.

### Flashen

1. **Hauptschalter SW1 aus** (der ESP darf nicht gleichzeitig über USB und den Buck-Wandler
   versorgt werden, siehe Schaltplan).
2. Board über die USB-C-Buchse **„TTL“** anschließen (nicht „OTG“). Erscheint unter Windows
   kein COM-Port im Geräte-Manager, den CH340/CH343-Treiber installieren
   (siehe Board-Doku, Seite „instructions“).
3. In der Statusleiste auf **→ (Upload)** klicken – oder `pio run -t upload`. PlatformIO
   findet den Port normalerweise selbst; sonst in `platformio.ini` z. B.
   `upload_port = COM7` ergänzen.
4. Falls „Failed to connect to ESP32-S3“ erscheint: **BOOT** gedrückt halten, kurz **RST**
   drücken, BOOT loslassen und den Upload wiederholen.
5. **Serieller Monitor:** Stecker-Symbol in der Statusleiste (oder `pio device monitor`),
   115200 Baud. Nach einem Druck auf **RST** erscheint z. B.:
   ```
   [Servo] PCA9685 #1 (0x40): OK, #2 (0x41): OK
   [IMU]   MPU6050 (0x68): OK
   [Akku]  0.42 V (Überwachung aus, Tiefschlaf an)
   [Kamera] OK (schläft bis zum ersten Stream)
   [WiFi] Access Point "Hexapod-MK3" aktiv, IP 192.168.4.1
   [Web]   http://192.168.4.1/  bzw.  http://hexapod.local/
   ```
   Ohne Servo-Versorgung (SW1 aus) werden die PCA9685 trotzdem erkannt – deren Logik hängt
   an den 3,3 V des ESP.

Gespeicherte Einstellungen (Kalibrierung, WLAN, Kamera, Akku-Schalter) bleiben bei einem
erneuten Upload erhalten.

### Alternative: fertige Firmware im Browser flashen

Jeder Push baut die Firmware automatisch auf GitHub (Workflow *Firmware*). Ohne lokale
Toolchain:

1. Auf GitHub **Actions → Firmware →** den neuesten grünen Lauf öffnen und unter *Artifacts*
   **hexapod-firmware** herunterladen (GitHub-Login nötig), ZIP entpacken.
2. In **Chrome oder Edge** <https://espressif.github.io/esptool-js/> öffnen, Board an „TTL“
   anschließen (SW1 aus) und **Connect** wählen, dann den COM-Port auswählen.
3. Unter *Flash Address* `0x0` eintragen, die Datei **`firmware.factory.bin`** auswählen und
   **Program** klicken. *Erase Flash* nicht nötig – sonst gehen gespeicherte Einstellungen
   (Kalibrierung, WLAN) verloren.
4. Danach **RST** drücken. Die serielle Ausgabe zeigt z. B. das Terminal derselben Seite
   (115200 Baud).

### Erster Start

1. **WLAN einrichten** (einmalig, ohne Dateien zu bearbeiten):
   1. Handy oder PC mit dem WLAN **„Hexapod-MK3“** verbinden (Passwort `hexapod123`).
      Die Oberfläche öffnet sich meist von selbst (Captive Portal), sonst `http://192.168.4.1`
      im Browser aufrufen.
   2. Auf **„WLAN einrichten“** tippen (Banner oben) bzw. Menü *Einstellungen* → *WLAN*.
   3. **„Netze suchen“**, das Heimnetz antippen, Passwort eingeben, **„Verbinden“**.
      Nur 2,4-GHz-Netze werden unterstützt.
   4. Nach wenigen Sekunden zeigt die Karte die **neue Adresse** im Heimnetz an
      (z. B. `http://192.168.178.57`). Das Gerät wieder mit dem Heimnetz verbinden und diese
      Adresse öffnen – am besten als Lesezeichen. Der Access Point schaltet sich danach ab.

   Die Zugangsdaten bleiben gespeichert. Ist das Heimnetz beim Start nicht erreichbar, startet
   wieder der Access Point; der Hexapod versucht das Heimnetz im Hintergrund weiter.
   Alternativ zur IP funktioniert oft auch `http://hexapod.local`.
2. Zum Bewegen der Servos braucht es den Akku: USB abziehen, SW1 ein, dann über WLAN
   weiterarbeiten.
3. Servos kalibrieren: Anleitung in [docs/kalibrierung.md](docs/kalibrierung.md), Lehre zum
   Aufstecken der Hörner: [docs/montagelehre.pdf](docs/montagelehre.pdf).

Nach dem Start sind alle Servos **stromlos**, bis in der Oberfläche *Torque* eingeschaltet wird.
Not-Aus jederzeit über **STOP** (oder Taste **Esc**).

Die **Akkuüberwachung** (Warnung, Servo-Abschaltung, Tiefschlaf) ist ab Werk **aus**. Erst
nach dem Bestücken des Spannungsteilers an GPIO 1 unter *Einstellungen* → *Akku* einschalten, siehe
[Hinweise zur Schaltung](docs/hardware.md#hinweise-zur-schaltung).

## Architektur

```
            Core 0                                   Core 1
 ┌─────────────────────────────┐      ┌───────────────────────────────────┐
 │ WLAN, mDNS                  │      │ Regel-Task, 50 Hz                 │
 │ HTTP :80  UI, REST, /ws ────┼─────►│  MPU6050 lesen                    │
 │           (setzt Sollwerte) │      │  Akku (10 Hz) → Schutz (optional) │
 │ HTTP :81  MJPEG-Stream      │      │  Pose-Modus: Haltung → IK         │
 │ Kamera-Treiber              │      │  Rampen → PCA9685 (einziger I²C-  │
 └─────────────────────────────┘      │  Nutzer, nur bei Änderungen)      │
                                      └───────────────────────────────────┘
 loop(): Telemetrie (4 Hz) an alle WebSocket-Clients, Status-LEDs, Sicherheitslogik
```

- Kamera-Stream auf eigenem Server (Port 81), damit ein laufender Stream die Steuerung nicht
  blockiert. Ohne Stream schläft die Kamera (der OV5640 wird im Dauerbetrieb sehr heiß).
- Die Oberfläche hat keine externen Abhängigkeiten und funktioniert daher auch im
  Access-Point-Modus ohne Internet.
- Alle Einstellungen (Kalibrierung, WLAN, Kamera) liegen im NVS und überleben Firmware-Updates.

## Oberfläche

Menü links (am PC einklappbar, am Handy über ☰), Not-Aus **STOP** (oder Taste **Esc**) immer
oben rechts. Jede Seite hat eine eigene Adresse, z. B. `http://hexapod.local/#kalibrierung`.

| Seite | Inhalt |
|---|---|
| **Steuerung** | Kamerastream (Vollbild, Lage als künstlicher Horizont im Bild, Kameraeinstellungen über ⚙), Laufen mit Joysticks bzw. W/A/S/D/Q/E (R/F: Höhe), Aufstehen/Hinsetzen, Körperhaltung (IK) mit Lageausgleich, Lage mit Nullpunkt, Torque |
| **Kalibrierung & Justierung** | *Kalibrieren*: Servo für Servo (Rohmodus, Montagestellung, Referenz, Grenzen, Test); *Justieren*: alle 18 Gelenke auf einen Blick; Speichern, Export/Import |
| **Systemübersicht** | Kennzahlen (Akku, WLAN, Regelung, Kamera, Speicher, Laufzeit), Komponenten, I²C-Scan, Neustart |
| **Einstellungen** | WLAN, Servo-Stellgeschwindigkeit, Lagesensor (Einbaulage, Nullpunkt, Gyro), Akkuüberwachung, Farbschema (automatisch/dunkel/hell), Tastenkürzel |

## Oberfläche ohne Roboter entwickeln

```
cd firmware
pip install aiohttp
python tools/mock_server.py      # http://localhost:8080
```

Der Mock simuliert API, WebSocket und Telemetrie; Änderungen in `web/` sind nach einem
Neuladen der Seite sichtbar. Beim nächsten Build bettet `scripts/embed_web.py` sie
automatisch in die Firmware ein.
