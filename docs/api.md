# Schnittstellen der Firmware

Die Weboberfläche nutzt ausschließlich diese Schnittstellen – eigene Clients (Python-Skripte,
später KI-Module am PC) können sie genauso verwenden. Alle Daten sind JSON.

| Dienst | Adresse |
|---|---|
| Oberfläche, REST, WebSocket | `http://hexapod.local/` bzw. `http://<IP>/` (Port 80) |
| Kamera MJPEG-Stream | `http://<IP>:81/stream` |
| Kamera Einzelbild | `http://<IP>:81/capture` |

Der Stream-Server bedient einen Stream gleichzeitig; ein weiterer Client wartet, bis der erste
die Verbindung schließt.

Kamera-Drehung: 180° macht der Sensor selbst. Bei 90°/270° liefert der Sensor 0°/180°, und die
Oberfläche dreht das Bild um weitere 90° – eigene Clients müssen das bei 90°/270° ebenfalls tun.

## REST

| Methode & Pfad | Body | Antwort |
|---|---|---|
| `GET /api/status` | – | Firmware, Chip, Speicher, WLAN, Kamera, Geräte, Akku |
| `GET /api/calibration` | – | `{"version":1,"servos":[…18…],"dirty":bool}` |
| `POST /api/calibration` | wie GET (ohne `dirty`) | übernimmt alle 18 Servos und **speichert sofort** |
| `GET /api/camera` | – | Einstellungen + verfügbare Auflösungen |
| `POST /api/camera` | `{"framesize","quality","vflip","hmirror","max_fps","rotation"}` (Teilmenge) | neue Einstellungen (persistent); `rotation` 0/90/180/270 |
| `GET /api/wifi` | – | WLAN-Status, `saved_ssid` |
| `POST /api/wifi` | `{"ssid","pass"}` oder `{"clear":true}` | wirksam nach Neustart |
| `GET /api/battery` | – | Akkuwerte und Schalter |
| `POST /api/battery` | `{"enabled","deep_sleep"}` (Teilmenge) | Akkuüberwachung / Tiefschlaf schalten (persistent) |
| `GET /api/kinematics` | – | Beinmaße, Montagepunkte, Grenzen der Körperhaltung, `placeholder` |
| `GET /api/i2c` | – | letzter I²C-Scan: `devices` (gefunden), `missing` (erwartet, fehlt) |
| `POST /api/i2c/scan` | `{}` | I²C-Bus neu scannen (0x08–0x77), Antwort wie GET |
| `POST /api/estop` | `{}` | Not-Aus (Fallback ohne WebSocket) |
| `POST /api/reboot` | `{}` | Neustart nach ~0,5 s |

Fehler: HTTP 4xx/5xx mit `{"ok":false,"msg":"…"}`.

## WebSocket `/ws`

### Befehle (Client → ESP32)

Jeder Befehl wird mit `{"type":"ack","cmd":…,"ok":bool,"msg"?:…}` beantwortet. Die Befehle
`servo_us`, `servo_deg`, `all_us` und `all_deg` schalten den Pose-Modus ab.

| `cmd` | Parameter | Wirkung |
|---|---|---|
| `ping` | – | Antwort `{"type":"pong"}` |
| `torque` | `on` (bool) | Servos ein (gestaffelt) / aus |
| `estop` | – | Not-Aus: alle Ausgänge sofort aus, Torque gesperrt |
| `estop_clear` | – | Not-Aus freigeben (Servos bleiben aus) |
| `mode` | `mode`: `"pose"` / `"manual"` | Körperhaltung per IK an/aus |
| `pose` | `x`, `y`, `z`, `roll`, `pitch`, `yaw`, `stance` (Teilmenge) | Zielhaltung (mm/Grad, wird begrenzt), siehe [kinematik.md](kinematik.md) |
| `pose_reset` | – | Standardhaltung |
| `servo_us` | `id`, `us` | Rohmodus: Pulsbreite 500–2500 µs |
| `servo_deg` | `id`, `deg` | kalibrierter Gelenkwinkel (auf Grenzen begrenzt) |
| `all_us` | `us` | alle Servos auf eine Pulsbreite (Montage: 1500) |
| `all_deg` | `deg` | alle Gelenke auf einen Winkel (Neutral: 0) |
| `slew` | `deg_per_s` | Rampengeschwindigkeit (10–1000 °/s) |
| `cal_get` | `id`? | einzelner Servo → `cal`, ohne `id` → `calibration` |
| `cal_set` | `id`, `center_us`?, `us_per_deg`?, `min_deg`?, `max_deg`? | Werte setzen (geprüft) |
| `cal_zero` | `id` | aktuelle Stellung als 0° übernehmen |
| `cal_ref` | `id`, `deg` | aktuelle Stellung als Referenzwinkel → `us_per_deg` |
| `cal_invert` | `id` | Drehrichtung umkehren |
| `cal_save` | – | Kalibrierung in NVS speichern |
| `cal_reload` | – | gespeicherte Kalibrierung laden (Änderungen verwerfen) |
| `cal_reset` | – | Standardwerte (noch nicht gespeichert) |

Beispiel:

```json
{"cmd": "servo_deg", "id": 4, "deg": 30}
```

### Telemetrie (ESP32 → alle Clients, alle 250 ms)

```json
{
  "type": "telemetry",
  "uptime": 123,
  "bat":   {"v": 7.62, "pct": 63, "state": "ok", "enabled": true, "deep_sleep": true},
  "servo": {"torque": true, "active": 18, "estop": false, "inhibit": "", "slew": 240,
            "cal_dirty": false, "us": [1500, …18…], "deg": [0.0, …18…], "boards": [true, true]},
  "imu":   {"ok": true, "pitch": 1.2, "roll": -0.4},
  "motion": {"mode": "pose", "pose": {"x": 0, "y": 0, "z": 72, "roll": 0, "pitch": 0, "yaw": 0,
             "stance": 90}, "unreachable": [], "clamped": []},
  "wifi":  {"mode": "sta", "rssi": -58},
  "heap": 182000,
  "ws": 1
}
```

`bat.state`: `off` (Überwachung ausgeschaltet – `v` ist dann nur ein ungeprüfter Messwert),
`usb` (kein Akku), `ok`, `warn`, `critical`. `servo.inhibit` enthält den Grund einer
Sperre (z. B. „Akku leer“). `motion.unreachable` / `motion.clamped` listen Beine (0–5), die ihren
Fußpunkt nicht erreichen bzw. an eine Gelenkgrenze stoßen.
