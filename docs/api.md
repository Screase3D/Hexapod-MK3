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

Ohne laufenden Stream bzw. Abruf schläft die Kamera nach 10 s (`CAM_IDLE_SLEEP_MS`): Sensor im
Software-Power-Down, Takt und Bildaufnahme aus – der OV5640 wird im Dauerbetrieb sonst sehr
heiß. Der nächste Abruf weckt sie (ca. 0,5–1 s bis zum ersten Bild).

Kamera-Drehung: 180° macht der Sensor selbst. Bei 90°/270° liefert der Sensor 0°/180°, und die
Oberfläche dreht das Bild um weitere 90° – eigene Clients müssen das bei 90°/270° ebenfalls tun.

## REST

| Methode & Pfad | Body | Antwort |
|---|---|---|
| `GET /api/status` | – | Firmware, Chip, Speicher, WLAN, Kamera, Geräte, Akku |
| `GET /api/calibration` | – | `{"version":2,"servos":[…18…],"dirty":bool}`, je Servo u. a. `ref_deg`, `center_us`, `us_per_deg`, `min_deg`, `max_deg` (siehe [kalibrierung.md](kalibrierung.md)) |
| `POST /api/calibration` | wie GET (ohne `dirty`) | übernimmt alle 18 Servos und **speichert sofort**; Dateien mit `version` 1 (`center_us` = Puls bei 0°) werden umgerechnet |
| `GET /api/camera` | – | Einstellungen, verfügbare Auflösungen, `sensor`, `awake` (Kamera wach) |
| `POST /api/camera` | `{"framesize","quality","vflip","hmirror","max_fps","rotation","xclk_mhz"}` (Teilmenge) | neue Einstellungen (persistent); `rotation` 0/90/180/270; `xclk_mhz` 10/16/20 wirkt beim nächsten Aufwecken |
| `GET /api/wifi` | – | WLAN-Status: `mode` (`ap`/`ap+sta`/`sta`), `ap` {active, ip, clients, off_in_s}, `sta` {state, ssid, ip, rssi, reason}, `saved_ssid` |
| `POST /api/wifi` | `{"ssid","pass","connect":true}` oder `{"clear":true}` | speichern; im AP-Modus mit `connect` sofort verbinden (Status über `GET /api/wifi` verfolgen), sonst nach Neustart |
| `POST /api/wifi/scan` | `{}` | Netzsuche starten (asynchron) |
| `GET /api/wifi/scan` | – | `running`, `networks` [{ssid, rssi, channel, secure}] |
| `GET /api/battery` | – | Akkuwerte und Schalter |
| `POST /api/battery` | `{"enabled","deep_sleep"}` (Teilmenge) | Akkuüberwachung / Tiefschlaf schalten (persistent) |
| `GET /api/kinematics` | – | Beinmaße, Montagepunkte, Grenzen der Körperhaltung (`limits`) und der Gangparameter (`gait_limits`), `placeholder` |
| `GET /api/i2c` | – | letzter I²C-Scan: `devices` (gefunden), `missing` (erwartet, fehlt) |
| `POST /api/i2c/scan` | `{}` | I²C-Bus neu scannen (0x08–0x77), Antwort wie GET |
| `POST /api/estop` | `{}` | Not-Aus (Fallback ohne WebSocket) |
| `POST /api/reboot` | `{}` | Neustart nach ~0,5 s |

Fehler: HTTP 4xx/5xx mit `{"ok":false,"msg":"…"}`.

## WebSocket `/ws`

### Befehle (Client → ESP32)

Jeder Befehl wird mit `{"type":"ack","cmd":…,"ok":bool,"msg"?:…}` beantwortet. Die Befehle
`servo_us`, `servo_deg`, `all_us`, `all_deg` und `all_assembly` schalten den Pose-Modus ab.

| `cmd` | Parameter | Wirkung |
|---|---|---|
| `ping` | – | Antwort `{"type":"pong"}` |
| `torque` | `on` (bool) | Servos ein (gestaffelt) / aus |
| `estop` | – | Not-Aus: alle Ausgänge sofort aus, Torque gesperrt |
| `estop_clear` | – | Not-Aus freigeben (Servos bleiben aus) |
| `mode` | `mode`: `"pose"` / `"manual"` | Körperhaltung per IK an/aus |
| `pose` | `x`, `y`, `z`, `roll`, `pitch`, `yaw`, `stance` (Teilmenge) | Zielhaltung (mm/Grad, wird begrenzt), siehe [kinematik.md](kinematik.md) |
| `pose_reset` | – | Standardhaltung |
| `walk` | `vx`, `vy`, `turn` (je −1…1) | Fahrbefehl: vor/zurück, links/rechts, drehen (+ = links). Schaltet in den IK-Modus. **Ohne Antwort**; ohne neuen Befehl für 0,6 s hält der Roboter an – also während der Fahrt etwa alle 100 ms senden |
| `stop` | – | Fahrbefehl auf 0 (Roboter läuft noch in die Neutralstellung) |
| `stand` | – | aufstehen (aus dem Sitzen; braucht Torque), schaltet in den IK-Modus |
| `sit` | – | hinsetzen |
| `gait` | `type` (`tripod`/`ripple`/`wave`), `swing_s`, `step_height` (Teilmenge) | Gangparameter, siehe [kinematik.md](kinematik.md#laufen); `type` wirkt im Stillstand |
| `servo_us` | `id`, `us` | Rohmodus: Pulsbreite 500–2500 µs |
| `servo_deg` | `id`, `deg` | kalibrierter Gelenkwinkel (auf Grenzen begrenzt) |
| `all_us` | `us` | alle Servos auf eine Pulsbreite (Montage: 1500) |
| `all_deg` | `deg` | alle Gelenke auf denselben Gelenkwinkel |
| `all_assembly` | – | alle Gelenke in die Montagestellung (= Standardhaltung) |
| `slew` | `deg_per_s` | Rampengeschwindigkeit (10–1000 °/s) |
| `cal_get` | `id`? | einzelner Servo → `cal`, ohne `id` → `calibration` |
| `cal_set` | `id`, `center_us`?, `us_per_deg`?, `min_deg`?, `max_deg`? | Werte setzen (geprüft) |
| `cal_center` | `id` | aktuelle Stellung als Montagestellung übernehmen (`center_us`) |
| `cal_ref` | `id`, `deg` | aktuelle Stellung als zweiter Bezugswinkel → `us_per_deg` (mind. 10° von der Montagestellung entfernt) |
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
  "motion": {"mode": "pose", "posture": "standing", "pose": {"x": 0, "y": 0, "z": 72.5, "roll": 0, "pitch": 0, "yaw": 0,
             "stance": 78.2},
             "gait": {"type": "tripod", "swing_s": 0.5, "step_height": 15, "active": "tripod",
                      "walking": true, "swing": 21, "vx": 48, "vy": 0, "wz": 0,
                      "max_speed": 80, "max_turn": 33},
             "unreachable": [], "clamped": []},
  "wifi":  {"mode": "sta", "rssi": -58},
  "cam":   {"clients": 1, "awake": true, "fps": 12.4, "kb": 23, "kbps": 2300, "err": 0,
            "wake_ms": 640, "wakes": 3},
  "heap": 182000,
  "ws": 1
}
```

`bat.state`: `off` (Überwachung ausgeschaltet – `v` ist dann nur ein ungeprüfter Messwert),
`usb` (kein Akku), `ok`, `warn`, `critical`. `servo.inhibit` enthält den Grund einer
Sperre (z. B. „Akku leer“). `motion.unreachable` / `motion.clamped` listen Beine (0–5), die ihren
Fußpunkt nicht erreichen bzw. an eine Gelenkgrenze stoßen. `motion.posture` (nur im IK-Modus):
`sitting`, `standing_up`, `standing`, `sitting_down` – gelaufen wird nur in `standing`.
`motion.gait`: `type` = gewählte,
`active` = laufende Gangart, `swing` = Bitmaske der Beine in der Luft (Bit 0 = Bein 1),
`vx`/`vy` in mm/s, `wz` in °/s, `max_*` = Höchstwerte bei den aktuellen Parametern.
