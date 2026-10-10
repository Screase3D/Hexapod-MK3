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

## Schnellstart

1. **VS Code** mit der Erweiterung **PlatformIO IDE** installieren
   (alternativ die Fork-Erweiterung *pioarduino IDE*).
2. Den Ordner **`firmware/`** in VS Code öffnen. Beim ersten Build lädt PlatformIO die
   Plattform [pioarduino](https://github.com/pioarduino/platform-espressif32)
   (Arduino-Core 3.3 auf ESP-IDF 5.5) – das dauert einige Minuten.
3. *Optional:* `include/secrets.example.h` nach `include/secrets.h` kopieren und WLAN-Daten
   eintragen. Ohne diese Datei startet der Hexapod einen eigenen Access Point.
4. Board über die USB-C-Buchse **„TTL“** (CH340) anschließen – unter Windows ggf. den
   CH343-Treiber aus der Board-Doku installieren. Hauptschalter SW1 dabei aus
   (siehe [Hinweise](docs/hardware.md#hinweise-zur-schaltung)).
5. **Upload** (PlatformIO: „Upload“ bzw. `pio run -t upload`), danach **Monitor**
   (115200 Baud). Der serielle Monitor zeigt erkannte Komponenten und die IP-Adresse.
6. Browser öffnen:
   - im Heimnetz: `http://hexapod.local` oder die angezeigte IP,
   - im Access-Point-Modus: mit WLAN **„Hexapod-MK3“** (Passwort `hexapod123`) verbinden,
     dann `http://192.168.4.1`. Im Tab *System* lassen sich die Heimnetz-Daten eintragen.
7. Servos kalibrieren: Anleitung in [docs/kalibrierung.md](docs/kalibrierung.md).

Nach dem Start sind alle Servos **stromlos**, bis in der Oberfläche *Torque* eingeschaltet wird.
Not-Aus jederzeit über **STOP** (oder Taste **Esc**).

## Architektur

```
            Core 0                                   Core 1
 ┌─────────────────────────────┐      ┌───────────────────────────────────┐
 │ WLAN, mDNS                  │      │ Regel-Task, 50 Hz                 │
 │ HTTP :80  UI, REST, /ws ────┼─────►│  MPU6050 lesen                    │
 │           (setzt Sollwerte) │      │  Akku (10 Hz) → Schutzabschaltung │
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
