# Hexapod-MK3

Dritte Version des 3D-gedruckten Hexapods – diesmal mit ESP32-S3-CAM.
18× MG90S an 2× PCA9685, MPU6050, Kamera, Steuerung per Browser über WLAN.

**Ziel:** natürliche, fließende Bewegungen mit inverser Kinematik, gesteuert über eine
Weboberfläche, die der ESP32 selbst ausliefert (PC und Smartphone), inklusive Kamerastream –
später erweitert um KI-Funktionen mit der Kamera.

## Stand

| Phase | Inhalt | Status |
|---|---|---|
| 1 | Grundgerüst: WLAN, Webserver, Kamerastream, Servotreiber, **Servo-Kalibrierung**, Akku-/Sicherheitsfunktionen | ✅ kompiliert, UI im Browser gegen Mock getestet – **noch nicht auf echter Hardware getestet** |
| 2 | Kinematik: Beingeometrie, inverse Kinematik je Bein, Körperhaltung (Höhe, Neigen, Drehen), Aufstehen/Hinsetzen | geplant |
| 3 | Gangarten (Tripod, Wave, Ripple) mit weichen Fußtrajektorien, Touch-Joysticks in der UI, Verbindungs-Watchdog | geplant |
| 4 | IMU-Stabilisierung (Körper waagerecht halten), Geländeanpassung | geplant |
| 5 | Kamera/KI: Objekterkennung (auf dem ESP32-S3 oder am PC über den Stream), Folgen, OTA-Updates | Ideen |

## Repository

```
docs/
  hexapod_schaltplan_v3.png          Verdrahtungsplan
  ESP32_S3_Camera_Instructions.pdf   Board-Doku (Pinout S. 4)
  hardware.md                        Pinbelegung, Servo-Zuordnung, Hinweise zur Schaltung
  kalibrierung.md                    Gelenk-Konvention und Kalibrierablauf
  api.md                             REST-, WebSocket- und Stream-Schnittstellen
firmware/                            PlatformIO-Projekt
  platformio.ini
  include/config.h                   alle Pins, Adressen, Grenzwerte
  src/main.cpp                       Start, Regel-Task, Sicherheitslogik
  src/servo/                         PCA9685-Treiber, Servo-Mapping, Kalibrierung, ServoController
  src/sensors/                       Akku, MPU6050
  src/net/                           WLAN, HTTP/WebSocket, Kamera, Befehle
  src/ui/                            Status-LEDs (Augen)
  web/                               Weboberfläche (HTML/CSS/JS, wird in die Firmware eingebettet)
  tools/mock_server.py               Oberfläche am PC ohne Roboter testen
```

## Kompilieren und Flashen

### Einmalig einrichten

1. **[Git](https://git-scm.com/download/win)** installieren – die ESP32-Plattform braucht es
   auch dann, wenn der Code als ZIP geladen wird (sonst: „Git not found in PATH“). Im
   Installer die Standardoption „Git from the command line and also from 3rd-party software“
   beibehalten. Alternativ in PowerShell: `winget install --id Git.Git -e`.
2. **[VS Code](https://code.visualstudio.com/)** installieren, darin die Erweiterung
   **PlatformIO IDE** (Erweiterungen-Ansicht, nach „PlatformIO“ suchen). Danach VS Code
   komplett schließen und neu starten, damit Git im PATH gefunden wird.
3. **Code holen** – am besten in einen kurzen Pfad ohne Leerzeichen, z. B. `C:\dev\` (Windows
   hat Probleme mit sehr langen Pfaden):
   ```
   git clone https://github.com/Screase3D/Hexapod-MK3.git
   ```
   Ohne Git: auf GitHub *Code* → *Download ZIP* und entpacken.
4. In VS Code **Datei → Ordner öffnen → `Hexapod-MK3/firmware`** wählen (der Ordner mit der
   `platformio.ini`, nicht das Repo-Hauptverzeichnis).
5. *Optional:* `include/secrets.example.h` nach `include/secrets.h` kopieren und die Daten des
   Heim-WLANs eintragen. Ohne diese Datei startet der Hexapod einen eigenen Access Point, in
   dem man das WLAN später über die Oberfläche einträgt.

### Kompilieren

- In der blauen Statusleiste unten auf **✓ (Build)** klicken – oder im Terminal `pio run`.
- Beim **ersten** Build lädt PlatformIO die Plattform
  [pioarduino](https://github.com/pioarduino/platform-espressif32) (Arduino-Core 3.3 auf
  ESP-IDF 5.5) samt Compiler. Das dauert je nach Internet 5–20 Minuten und belegt rund
  **4 GB** Platz. Danach dauert ein Build unter einer Minute.
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
   [Kamera] OK
   [WiFi] Access Point "Hexapod-MK3" aktiv, IP 192.168.4.1
   [Web]   http://192.168.4.1/  bzw.  http://hexapod.local/
   ```
   Ohne Servo-Versorgung (SW1 aus) werden die PCA9685 trotzdem erkannt – deren Logik hängt
   an den 3,3 V des ESP.

Gespeicherte Einstellungen (Kalibrierung, WLAN, Kamera, Akku-Schalter) bleiben bei einem
erneuten Upload erhalten.

### Erster Start

1. Browser öffnen:
   - im Heimnetz: `http://hexapod.local` oder die im Monitor angezeigte IP,
   - im Access-Point-Modus: mit dem WLAN **„Hexapod-MK3“** (Passwort `hexapod123`) verbinden,
     dann `http://192.168.4.1`. Im Tab *System* lassen sich die Heimnetz-Daten eintragen.
2. Zum Bewegen der Servos braucht es den Akku: USB abziehen, SW1 ein, dann über WLAN
   weiterarbeiten.
3. Servos kalibrieren: Anleitung in [docs/kalibrierung.md](docs/kalibrierung.md).

Nach dem Start sind alle Servos **stromlos**, bis in der Oberfläche *Torque* eingeschaltet wird.
Not-Aus jederzeit über **STOP** (oder Taste **Esc**).

Die **Akkuüberwachung** (Warnung, Servo-Abschaltung, Tiefschlaf) ist ab Werk **aus**. Erst
nach dem Bestücken des Spannungsteilers an GPIO 1 im Tab *System* einschalten, siehe
[Hinweise zur Schaltung](docs/hardware.md#hinweise-zur-schaltung).

## Architektur

```
            Core 0                                   Core 1
 ┌─────────────────────────────┐      ┌───────────────────────────────────┐
 │ WLAN, mDNS                  │      │ Regel-Task, 50 Hz                 │
 │ HTTP :80  UI, REST, /ws ────┼─────►│  MPU6050 lesen                    │
 │           (setzt Sollwerte) │      │  Akku (10 Hz) → Schutz (optional) │
 │ HTTP :81  MJPEG-Stream      │      │  (Phase 2: Gangart + IK)          │
 │ Kamera-Treiber              │      │  Rampen → PCA9685 (einziger I²C-  │
 └─────────────────────────────┘      │  Nutzer, nur bei Änderungen)      │
                                      └───────────────────────────────────┘
 loop(): Telemetrie (4 Hz) an alle WebSocket-Clients, Status-LEDs, Sicherheitslogik
```

- Kamera-Stream auf eigenem Server (Port 81), damit ein laufender Stream die Steuerung nicht
  blockiert.
- Die Oberfläche hat keine externen Abhängigkeiten und funktioniert daher auch im
  Access-Point-Modus ohne Internet.
- Alle Einstellungen (Kalibrierung, WLAN, Kamera) liegen im NVS und überleben Firmware-Updates.

## Oberfläche ohne Roboter entwickeln

```
cd firmware
pip install aiohttp
python tools/mock_server.py      # http://localhost:8080
```

Der Mock simuliert API, WebSocket und Telemetrie; Änderungen in `web/` sind nach einem
Neuladen der Seite sichtbar. Beim nächsten Build bettet `scripts/embed_web.py` sie
automatisch in die Firmware ein.
