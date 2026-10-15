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
| `GET /api/status` | – | `fw` (Version aus `firmware/VERSION`), `git` (Commit, `+` = mit lokalen Änderungen gebaut), `build` (Datum), `ui` (Kennung der eingebetteten Oberfläche), Chip, Speicher, WLAN, Kamera, Geräte, Akku |
| `GET /api/calibration` | – | `{"version":2,"servos":[…18…],"dirty":bool}`, je Servo u. a. `ref_deg`, `center_us`, `us_per_deg`, `min_deg`, `max_deg` (siehe [kalibrierung.md](kalibrierung.md)) |
| `POST /api/calibration` | wie GET (ohne `dirty`) | übernimmt alle 18 Servos und **speichert sofort**; Dateien mit `version` 1 (`center_us` = Puls bei 0°) werden umgerechnet. Schaltet in den manuellen Modus (siehe `cal_*`) |
| `GET /api/camera` | – | Einstellungen, verfügbare Auflösungen, `sensor`, `awake` (Kamera wach), `bt_framesize`, `framesize_active`, `image` {…}, `sharpness_ok`, `zoom` (aktuell), `zoom_max` (bei dieser Auflösung), `zoom_hw` (Zoom am Sensor) |
| `POST /api/camera` | `{"framesize","quality","vflip","hmirror","max_fps","rotation","xclk_mhz","bt_framesize"}` und Bild: `brightness`, `contrast`, `saturation`, `sharpness`, `ae_level` (je −2..2), `gainceiling` (0..6 = 2×..128×), `wb_mode` (0 auto, 1 Sonne, 2 Wolken, 3 Büro, 4 Zuhause), `aec2`, `lenc`, `bpc` (bool) – jeweils Teilmenge | neue Einstellungen (persistent); `rotation` 0/90/180/270; `xclk_mhz` 10/16/20 wirkt beim nächsten Aufwecken; `bt_framesize` = höchste Auflösung, solange Bluetooth an ist (0 = keine Anpassung). Schärfe nur OV5640/OV3660. `zoom` 1..4 (auf `zoom_max` begrenzt): OV5640 liest einen Ausschnitt des Sensors, wird nicht gespeichert; `{"zoom"}` allein schreibt nichts in den Flash |
| `GET /api/wifi` | – | WLAN-Status: `mode` (`ap`/`ap+sta`/`sta`), `ap` {active, ip, clients, off_in_s}, `sta` {state, ssid, ip, rssi, reason}, `saved_ssid` |
| `POST /api/wifi` | `{"ssid","pass","connect":true}` oder `{"clear":true}` | speichern; im AP-Modus mit `connect` sofort verbinden (Status über `GET /api/wifi` verfolgen), sonst nach Neustart |
| `POST /api/wifi/scan` | `{}` | Netzsuche starten (asynchron) |
| `GET /api/wifi/scan` | – | `running`, `networks` [{ssid, rssi, channel, secure}] |
| `GET /api/battery` | – | Akkuwerte und Schalter |
| `POST /api/battery` | `{"enabled","deep_sleep"}` (Teilmenge) | Akkuüberwachung / Tiefschlaf schalten (persistent) |
| `GET /api/leds` | – | Augen-LEDs: `mode` (`auto`/`single`/`off`), `brightness` (5–100 %), `board` (`boot`/`mirror`/`off`), `single` {color, effect}, `states` (einstellbar: ap, idle, torque, stand, walk, arms, gesture) und `fixed` (boot, wifi, bat_warn, bat_low, estop) je {id, color `#rrggbb`, effect}, `state` (gerade aktiv) |
| `POST /api/leds` | Teilmenge: `mode`, `brightness`, `board`, `single` {color?, effect?}, `state`+`color?`+`effect?`, `reset`, `preview` (zeigt den geänderten Stil 2,5 s) | wie GET; gespeichert. Effekte: `off`, `solid`, `breathe`, `blink`, `pulse`, `wink` (Blinzeln), `alternate`, `rainbow`, `flicker` (Flackern), `strobe` (Blitzen), `scan` (hin und her), `sleepy` (müde) |
| `GET /api/pad` | – | Controller: `enabled` (Bluetooth), `addr` (gekoppelt), `deadzone`, `expo`, `speed` (0..1), `invert_y`, `swap_sticks`, `map` {Taste: Aktion}, `buttons`, `actions` (siehe [controller.md](controller.md)) |
| `POST /api/pad` | Teilmenge: `enabled`, `deadzone`, `expo`, `speed`, `invert_y`, `swap_sticks`, `map` {Taste: Aktion}, `pair`, `forget`, `reset` | wie GET; gespeichert. `pair` schaltet Bluetooth ein und sucht 60 s |
| `GET /api/history` | – | Verlauf der letzten 30 min (alle `period_s` = 10 s, älteste zuerst): `bat` (V), `rssi` (dBm), `temp` (Chip, °C), `heap` (KB); `null` = kein Wert; `age_ms` seit dem neuesten |
| `GET /api/log?since=N` | – | Ereignisprotokoll: `seq` (neueste Nummer), `uptime_ms`, `entries` [{seq, ms (seit Start), lvl (`info`/`warn`/`error`), text}] mit Nummer > N |
| `POST /api/ota` | Firmware (`firmware.bin`) als Body, `application/octet-stream` | schreibt die Firmware in die zweite App-Partition (Servos vorher stromlos), prüft sie und startet neu; Fehler mit `msg` |
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
`pose`, `pose_reset`, `gait`, `leveling`, `alive` und `slew` werden gespeichert (NVS, 2 s nach der letzten
Änderung) und gelten nach einem Neustart weiter.

| `cmd` | Parameter | Wirkung |
|---|---|---|
| `ping` | – | Antwort `{"type":"pong"}` |
| `torque` | `on` (bool) | Servos ein (gestaffelt) / aus |
| `estop` | – | Not-Aus: alle Ausgänge sofort aus, Torque gesperrt |
| `estop_clear` | – | Not-Aus freigeben (Servos bleiben aus) |
| `mode` | `mode`: `"pose"` / `"manual"` | Körperhaltung per IK an/aus (wird gespeichert und gilt nach einem Neustart wieder; beginnt in der Haltung, in der er gerade ist – nie Sprung in die Standhaltung) |
| `pose` | `x`, `y`, `z`, `roll`, `pitch`, `yaw`, `stance` (Teilmenge) | Zielhaltung (mm/Grad, wird begrenzt), siehe [kinematik.md](kinematik.md). Ist sie nicht erreichbar, gilt die nächste erreichbare; die Antwort enthält dann `"limited": true` und in `msg`, was begrenzt wurde |
| `pose_reset` | – | Standardhaltung |
| `walk` | `vx`, `vy`, `turn` (je −1…1) | Fahrbefehl: vor/zurück, links/rechts, drehen (+ = links). Wirkt nur im IK-Modus (vorher `stand`). **Ohne Antwort**; ohne neuen Befehl für 0,6 s hält der Roboter an – also während der Fahrt etwa alle 100 ms senden |
| `stop` | – | Fahrbefehl auf 0 (Roboter läuft noch in die Neutralstellung) |
| `stand` | – | aufstehen (aus dem Sitzen; braucht Torque), schaltet in den IK-Modus |
| `sit` | – | hinsetzen (setzt erhobene Vorderbeine vorher ab) |
| `body` | `x`, `y`, `roll`, `pitch`, `yaw` (je −1…1) | Körper-Joystick: Körper verschieben (±30 mm) und neigen (±12°, Gieren ±15°), Füße bleiben stehen. **Ohne Antwort**, wie `walk` etwa alle 100 ms senden; ohne neuen Befehl federt der Körper nach 0,6 s zurück |
| `gesture` | `name`: `circle`/`nod`/`shake`/`pushup`/`dance`/`stop` | Geste abspielen bzw. weich beenden (nur im Stand, Vorderbeine unten), siehe [kinematik.md](kinematik.md#körper-bewegen-und-gesten) |
| `arms` | `up` (bool) | Vorderbeine heben (Gewicht nach hinten, dann heben; nur im Stillstand) bzw. absetzen, siehe [kinematik.md](kinematik.md#vorderbeine) |
| `arm` | `lx`, `lz`, `rx`, `rz`, `reach`? (je −1…1) | Vorderfüße steuern (l = Bein 1, r = Bein 4): x = um die Coxa schwenken, + = nach innen/vorne; z = hoch (+)/runter; `reach` = beide weiter weg (+)/heran. Nur wenn oben; der Fuß geht so weit, wie das Bein reicht. **Ohne Antwort**, etwa alle 100 ms senden; ohne Befehl kehren sie nach 0,6 s in die Bereitstellung zurück |
| `pad` | `lx`, `ly`, `rx`, `ry` (−1…1, y oben +), `lt`, `rt` (0…1), `b` (Tasten-Bits: a, b, x, y, lb, rb, view, menu, xbox, ls, rs, up, down, left, right, share, p1–p4) | Gamepad im Browser, wertet die Firmware wie einen Bluetooth-Controller aus. **Ohne Antwort**, etwa alle 50 ms senden; nach 0,3 s ohne Befehl gilt er als getrennt |
| `live` | `on` (bool, Standard `true`) | Live-Ansicht: dieser Client bekommt 3 s lang `{"type":"live"}` mit 20 Hz (siehe unten). **Ohne Antwort**; die Oberfläche erneuert das alle 1,5 s, solange die 3D-Ansicht sichtbar ist, `on: false` beendet es sofort |
| `alive` | `on` (bool) | „Lebendig“: im Stand ab und zu kleine zufällige Bewegungen, siehe [kinematik.md](kinematik.md#lebendig); wird gespeichert |
| `life` | `breathe`, `bored`, `lift`, `walk_fx`, `gaze` (bool), `sleep_min` (1–60), `lift_sens` (1 gering, 2 mittel, 3 hoch) – Teilmenge | „Lebendig wirken“: Atmen, Langeweile/Einschlafen nach `sleep_min`, Zappeln beim Hochheben, Laufen mit Charakter, Augen schauen mit, siehe [kinematik.md](kinematik.md#lebendig-wirken); wird gespeichert |
| `wave` | – | rechtes Vorderbein winkt (nur wenn oben) |
| `gait` | `type` (`tripod`/`ripple`/`wave`), `swing_s`, `step_height` (Teilmenge) | Gangparameter, siehe [kinematik.md](kinematik.md#laufen); `type` wechselt auch im Lauf fließend (`motion.gait.switching` = Übergang läuft) |
| `leveling` | `on` (bool) | Lageausgleich an/aus, siehe [kinematik.md](kinematik.md#lageausgleich) |
| `imu_cal` | `level` (bool) | Lagesensor kalibrieren (2 s ruhig halten): Gyro-Nullpunkt, mit `level` zusätzlich aktuelle Lage = waagerecht. Fortschritt/Ergebnis in der Telemetrie |
| `imu_level_reset` | – | Nullpunkt (waagerecht) löschen |
| `imu_mount` | `x` (`vorne`/`links`/`hinten`/`rechts`), `upside_down` (bool) | Einbaulage des Lagesensors (löscht den Nullpunkt) |
| `servo_us` | `id`, `us` | Rohmodus: Pulsbreite 500–2500 µs |
| `servo_deg` | `id`, `deg` | kalibrierter Gelenkwinkel (auf Grenzen begrenzt) |
| `all_us` | `us` | alle Servos auf eine Pulsbreite (Montage: 1500) |
| `all_deg` | `deg` | alle Gelenke auf denselben Gelenkwinkel |
| `all_assembly` | – | alle Gelenke in die Montagestellung (= Standardhaltung) |
| `slew` | `deg_per_s` | Rampengeschwindigkeit bei Einzelbefehlen (10–1000 °/s, wird gespeichert) |
| `cal_get` | `id`? | einzelner Servo → `cal`, ohne `id` → `calibration` |
| `cal_set` | `id`, `center_us`?, `us_per_deg`?, `min_deg`?, `max_deg`? | Werte setzen (geprüft) |
| `cal_center` | `id` | aktuelle Stellung als Montagestellung übernehmen (`center_us`) |
| `cal_ref` | `id`, `deg` | aktuelle Stellung als zweiter Bezugswinkel → `us_per_deg` (mind. 10° von der Montagestellung entfernt) |
| `cal_invert` | `id` | Drehrichtung umkehren |
| `cal_save` | – | Kalibrierung in NVS speichern |
| `cal_reload` | – | gespeicherte Kalibrierung laden (Änderungen verwerfen) |
| `cal_reset` | – | Standardwerte (noch nicht gespeichert) |
| `eyes` | `color` (`#rrggbb`), `effect`, `ms` (100–120000, Standard 3000) bzw. `reset: true` | Augen kurz vorgeben (Seite Verhalten): Farbe und Effekt wie auf der Seite LEDs, nicht über Warnungen (Not-Aus, Akku); `reset` = wieder je Zustand. Solange die Vorgabe gilt, steht sie in der Telemetrie unter `eyes` |
| `client_log` | `text` (bis 100 Zeichen) | Meldung der Oberfläche ins Ereignisprotokoll (Warnung), höchstens alle 10 s, dazwischen gezählt. **Ohne Antwort** |

Alle Befehle, die die Kalibrierung ändern (`cal_set`, `cal_center`, `cal_ref`, `cal_invert`, `cal_reload`,
`cal_reset`), schalten in den manuellen Modus: Im IK-Modus würde die neue Zuordnung Winkel → Puls alle
betroffenen Gelenke springen lassen. Im manuellen Modus bleiben die Pulse stehen; beim Zurückschalten
in den IK-Modus fährt die Haltung weich an.

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
  "imu":   {"ok": true, "pitch": 1.2, "roll": -0.4, "acc": [0.02, -0.01, 1.0], "hz": 50, "temp": 31.2,
            "mount": "vorne", "flip": false, "level": [-2.0, 1.5], "bias_src": "beim Start gemessen",
            "cal": "running", "cal_pct": 40},
  "motion": {"mode": "pose", "posture": "standing", "pose": {"x": 0, "y": 0, "z": 52, "roll": 0, "pitch": 0, "yaw": 0,
             "stance": 100},
             "range": {"z": [35, 100], "stance": [75, 135], "x": [-40, 40], "y": [-40, 40],
                       "roll": [-9.8, 9.8], "pitch": [-10.1, 10.1], "yaw": [-25, 25]},
             "level": {"on": true, "active": true, "roll": -3.2, "pitch": 1.1},
             "alive": {"on": true, "act": "look"},
             "life": {"breathe": true, "bored": true, "lift": true, "walk_fx": true, "gaze": true, "sleep_min": 5,
                      "lift_sens": 2,
                      "mood": "awake"},
             "gait": {"type": "tripod", "swing_s": 0.4, "step_height": 15, "active": "tripod",
                      "walking": true, "swing": 21, "vx": 100, "vy": 0, "wz": 0,
                      "max_speed": 125, "max_side": 95, "max_turn": 39, "nominal": 125, "cadence": 1.0},
             "arms": "down", "gesture": "nod", "body": true, "offset_limited": true,
             "unreachable": [], "clamped": []},
  "led":   "stand",
  "eyes":  {"color": "#ff2000", "effect": "flicker"},
  "log":   {"seq": 12, "warn": 3},
  "pad":   {"src": "ble", "mode": "walk", "speed": 60, "ble": "connected", "name": "Xbox Wireless Controller",
            "bat": 80, "in": {"lx": 0, "ly": 0.5, "rx": 0, "ry": 0, "lt": 0, "rt": 0, "b": 0}, "msg": "Modus: Laufen", "seq": 3},
  "wifi":  {"mode": "sta", "rssi": -58},
  "cam":   {"clients": 1, "awake": true, "fps": 12.4, "kb": 23, "kbps": 2300, "err": 0,
            "wake_ms": 640, "wakes": 3},
  "heap": 182000,
  "ws": 1,
  "ws_drop": 0,
  "ctl": {"hz": 50.0, "max_us": 2100}
}
```

`ws` = verbundene WebSocket-Clients, `ws_drop` = seit dem Start ausgelassene Telemetrie- und
Live-Nachrichten: Ist der Sendepuffer eines Clients voll (WLAN kommt nicht nach), lässt der Roboter
diese regelmäßigen Nachrichten für ihn aus, statt zu warten – so bleiben eingehende Befehle (Fahren!)
nicht hängen. Die Oberfläche zeigt die Anzahl der letzten Minute im Tooltip der Verbindungsanzeige.
Kommt ein Client 10 s lang gar nicht mehr nach (Verbindung tot, aber noch nicht abgemeldet), trennt ihn
der Roboter; die Oberfläche verbindet sich dann neu.

`ctl`: Regel-Task (Gang, IK, Servos) – erreichte Frequenz `hz` (Soll 50) und längste Rechenzeit eines
Zyklus in der letzten Sekunde `max_us`. `cam.bt_fs`: nur bei verbundenem Bluetooth-Controller, wenn die
Kamera deshalb mit kleinerer Auflösung läuft (Kennung wie `framesize` in `GET /api/camera`).

`bat.state`: `off` (Überwachung ausgeschaltet – `v` ist dann nur ein ungeprüfter Messwert),
`usb` (kein Akku), `ok`, `warn`, `critical`. `servo.inhibit` enthält den Grund einer
Sperre (z. B. „Akku leer“). `motion.unreachable` / `motion.clamped` listen Beine (0–5), die ihren
Fußpunkt nicht erreichen bzw. an eine Gelenkgrenze stoßen. `motion.posture` (nur im IK-Modus):
`sitting`, `standing_up`, `standing`, `sitting_down` – gelaufen wird nur in `standing`.
`motion.range`: für jede Größe der Zielhaltung der Bereich, den die Beine bei den übrigen eingestellten
Werten noch erreichen (Gelenkgrenzen der Kalibrierung eingeschlossen) – die Oberfläche zeigt ihn als Band
unter dem Regler und lässt den Regler nur bis dahin ziehen. Wird nur bei geänderter Zielhaltung bzw. alle
2 s neu berechnet; ist die Zielhaltung selbst nicht erreichbar, stehen hier die festen Grenzen.
`motion.gait`: `type` = gewählte,
`active` = laufende Gangart, `switching` = Gangwechsel im Lauf noch nicht abgeschlossen (nur wenn `true`), `swing` = Bitmaske der Beine in der Luft (Bit 0 = Bein 1),
`vx`/`vy` in mm/s, `wz` in °/s, `max_*` = Höchstwerte bei den aktuellen Parametern (geradeaus
bzw. seitwärts bzw. Drehen auf der Stelle, mit Reichweitengrenze), `nominal` = Tempo ohne
Reichweitengrenze (ist `max_speed` deutlich kleiner, schränkt die Haltung das Laufen ein),
`cadence` = aktuelle Schrittdauer / eingestellte (1–2, langsam = längere Schritte). `motion.arms`:
`down`, `shift_back`, `raise`, `up`, `lower`, `shift_forward`; `motion.gesture` nur während einer
Geste; `motion.body` solange der Körper-Joystick wirkt, `offset_limited` wenn er bzw. ein Vorderbein an
der Reichweitengrenze ist. `motion.alive`: Einstellung `on`, `act` = laufende Aktion (`look`, `shift`,
`tilt`, `lift`, `tap`, `shuffle`, bei Langeweile/Aufwachen `yawn`, `sigh`, `stretch`). `motion.life`: Einstellungen,
`mood` = `awake`, `bored` oder `asleep`, `flail: true` solange er zappelt. `led`: Zustand der Augen-LEDs (wie `GET /api/leds`). `pad`: Quelle
(`ble`, `web` oder leer), Modus, Tempo in %, Bluetooth-Zustand (`off`, `idle`, `searching`, `pairing` mit
`pair_left`, `connecting`, `connected` mit `name`, `bat`), `in` = aktueller Controller-Zustand (nur wenn aktiv),
`msg`/`seq` = letzte Meldung (neue Meldung = neue `seq`). `log`: Nummer des neuesten Eintrags im
Ereignisprotokoll und der neuesten Warnung (für den Hinweis im Menü). `motion.level`: Lageausgleich `on`, `active`
(wirkt gerade), aktuelle Korrektur `roll`/`pitch` in Grad, `fault: true` = wegen falscher
Sensorrichtung abgeschaltet. `imu`: `pitch`/`roll` nach Abzug des Nullpunkts `level`
([Roll, Nick] der schief eingebauten Platine), `acc` im Körpersystem, `mount`/`flip` =
Einbaulage; `cal` (`running`/`ok`/`failed`) mit `cal_pct` bzw. `cal_msg` nur während bzw. kurz nach
einer Kalibrierung.

### Live-Ansicht (ESP32 → Clients mit `live`, alle 50 ms)

```json
{"type": "live", "t": 123456, "j": [0, 554, -338, …18…], "tq": true, "i": [-12, 8],
 "ik": true, "b": [0, 0, 52, 0, 0, 0], "s": 21, "v": [100, 0, 15]}
```

`j` = aktuelle Sollwinkel der 18 Gelenke (mit Rampe) in 0,1° (Bein × 3 + Coxa/Femur/Tibia, Konvention
wie [kinematik.md](kinematik.md)), `tq` = Torque, `i` = gemessene Lage [Rollen, Nicken] in 0,1° (nur mit
Lagesensor), `ik` = IK-Betrieb. Nur im IK-Betrieb: `b` = Körperlage der letzten IK [x, y, z in mm, Rollen,
Nicken, Gieren in Grad] (mit Lageausgleich, Körper-Joystick, Gesten), `s` = Beine in der Luft (Bitmaske),
`v` = Tempo [vx, vy in mm/s, Drehen in °/s]. Die Oberfläche rechnet daraus mit der Vorwärtskinematik das
3D-Modell.
