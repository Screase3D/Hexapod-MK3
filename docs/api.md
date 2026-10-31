# Firmware interfaces

The web UI uses these interfaces exclusively – your own clients (Python scripts,
later AI modules on a PC) can use them in exactly the same way. All data is JSON.

| Service | Address |
|---|---|
| UI, REST, WebSocket | `http://hexapod.local/` or `http://<IP>/` (port 80) |
| Camera MJPEG stream | `http://<IP>:81/stream` |
| Camera single frame | `http://<IP>:81/capture` |

The stream server serves one stream at a time; another client waits until the first one
closes the connection.

With no stream or capture running, the camera goes to sleep after 10 s (`CAM_IDLE_SLEEP_MS`): sensor in
software power-down, clock and image capture off – otherwise the OV5640 gets very hot in continuous
operation. The next request wakes it (approx. 0.5–1 s until the first frame).

Camera rotation: the sensor handles 180° itself. For 90°/270° the sensor delivers 0°/180°, and the
UI rotates the image by a further 90° – your own clients must do the same for 90°/270°.

## REST

| Method & path | Body | Response |
|---|---|---|
| `GET /api/status` | – | `fw` (version from `firmware/VERSION`), `git` (commit, `+` = built with local changes), `build` (date), `ui` (identifier of the embedded UI), chip, memory, Wi-Fi, camera, devices, battery |
| `GET /api/calibration` | – | `{"version":2,"servos":[…18…],"dirty":bool}`, per servo among others `ref_deg`, `center_us`, `us_per_deg`, `min_deg`, `max_deg` (see [kalibrierung.md](kalibrierung.md)) |
| `POST /api/calibration` | as GET (without `dirty`) | applies all 18 servos and **saves immediately**; files with `version` 1 (`center_us` = pulse at 0°) are converted. Switches to manual mode (see `cal_*`) |
| `GET /api/camera` | – | settings, available resolutions, `sensor`, `awake` (camera awake), `bt_framesize`, `framesize_active`, `image` {…}, `sharpness_ok`, `zoom` (current), `zoom_max` (at this resolution), `zoom_hw` (zoom and cropping on the sensor), `crop`, `crop_edge`, `out_w`/`out_h` (image size of the stream, smaller with cropping on the sensor) |
| `POST /api/camera` | `{"framesize","quality","vflip","hmirror","max_fps","rotation","xclk_mhz","bt_framesize"}` and image: `brightness`, `contrast`, `saturation`, `sharpness`, `ae_level` (each −2..2), `gainceiling` (0..6 = 2×..128×), `wb_mode` (0 auto, 1 sun, 2 cloudy, 3 office, 4 home), `aec2`, `lenc`, `bpc` (bool) – any subset | new settings (persistent); `rotation` 0/90/180/270; `xclk_mhz` 10/16/20 takes effect on the next wake-up; `bt_framesize` = highest resolution while Bluetooth is on (0 = no adjustment). Sharpness only on OV5640/OV3660. `zoom` 1..4 (limited to `zoom_max`): the OV5640 reads out a window of the sensor, not saved; `{"zoom"}` on its own writes nothing to flash. `crop` 0..50 (%) and `crop_edge` (`bottom`/`top`, in the displayed image): crop off an edge (persistent) – the image becomes shorter by the strip, nothing is enlarged; the OV5640 does not read out the strip (e.g. 640×320 instead of 640×480), otherwise the UI hides it |
| `GET /api/wifi` | – | Wi-Fi status: `mode` (`ap`/`ap+sta`/`sta`), `ap` {active, ip, clients, off_in_s}, `sta` {state, ssid, ip, rssi, reason}, `saved_ssid`, `ps` (power saving on – only while Bluetooth is running) |
| `POST /api/wifi` | `{"ssid","pass","connect":true}` or `{"clear":true}` | save; in AP mode with `connect`, connect immediately (follow the status via `GET /api/wifi`), otherwise after a restart |
| `POST /api/wifi/scan` | `{}` | start network scan (asynchronous) |
| `GET /api/wifi/scan` | – | `running`, `networks` [{ssid, rssi, channel, secure}] |
| `GET /api/battery` | – | battery values and switches |
| `POST /api/battery` | `{"enabled","deep_sleep"}` (subset) | switch battery monitoring / deep sleep (persistent) |
| `GET /api/leds` | – | eye LEDs: `mode` (`auto`/`single`/`off`), `brightness` (5–100 %), `board` (`boot`/`mirror`/`off`), `single` {color, effect}, `states` (configurable: ap, idle, torque, stand, walk, arms, gesture) and `fixed` (boot, wifi, bat_warn, bat_low, estop), each {id, color `#rrggbb`, effect}, `state` (currently active) |
| `POST /api/leds` | subset: `mode`, `brightness`, `board`, `single` {color?, effect?}, `state`+`color?`+`effect?`, `reset`, `preview` (shows the changed style for 2.5 s) | as GET; saved. Effects: `off`, `solid`, `breathe`, `blink`, `pulse`, `wink` (wink), `alternate`, `rainbow`, `flicker` (flicker), `strobe` (strobe), `scan` (back and forth), `sleepy` (tired), `blink_fast` (fast blink, 4 Hz) |
| `GET /api/pad` | – | controller: `enabled` (Bluetooth), `addr` (paired), `deadzone`, `expo`, `speed` (0..1), `invert_y`, `swap_sticks`, `map` {button: action}, `buttons`, `actions` (see [controller.md](controller.md)) |
| `POST /api/pad` | subset: `enabled`, `deadzone`, `expo`, `speed`, `invert_y`, `swap_sticks`, `map` {button: action}, `pair`, `forget`, `reset` | as GET; saved. `pair` switches Bluetooth on and searches for 60 s |
| `GET /api/history` | – | history of the last 30 min (every `period_s` = 10 s, oldest first): `bat` (V), `rssi` (dBm), `temp` (chip, °C), `heap` (KB); `null` = no value; `age_ms` since the newest |
| `GET /api/log?since=N` | – | event log: `seq` (newest number), `uptime_ms`, `entries` [{seq, ms (since start), lvl (`info`/`warn`/`error`), text}] with number > N |
| `POST /api/ota` | firmware (`firmware.bin`) as body, `application/octet-stream` | writes the firmware to the second app partition (servos de-energised first), verifies it and restarts; errors with `msg` |
| `GET /api/kinematics` | – | leg dimensions, mounting points, limits of the body posture (`limits`) and of the gait parameters (`gait_limits`), `placeholder` |
| `GET /api/i2c` | – | last I²C scan: `devices` (found), `missing` (expected, missing) |
| `POST /api/i2c/scan` | `{}` | rescan the I²C bus (0x08–0x77), response as GET |
| `POST /api/estop` | `{}` | emergency stop (fallback without WebSocket) |
| `POST /api/reboot` | `{}` | restart after ~0.5 s |

Errors: HTTP 4xx/5xx with `{"ok":false,"msg":"…"}`.

## WebSocket `/ws`

### Commands (client → ESP32)

Every command is answered with `{"type":"ack","cmd":…,"ok":bool,"msg"?:…}`. The commands
`servo_us`, `servo_deg`, `all_us`, `all_deg` and `all_assembly` switch pose mode off.
`pose`, `pose_reset`, `gait`, `leveling`, `alive` and `slew` are saved (NVS, 2 s after the last
change) and remain in effect after a restart.

| `cmd` | Parameters | Effect |
|---|---|---|
| `ping` | – | response `{"type":"pong"}` |
| `torque` | `on` (bool) | servos on (staggered) / off |
| `estop` | – | emergency stop: all outputs off immediately, torque locked |
| `estop_clear` | – | release emergency stop (servos stay off) |
| `mode` | `mode`: `"pose"` / `"manual"` | body posture via IK on/off (saved and in effect again after a restart; starts from the posture the robot is currently in – never jumps to the standing posture) |
| `pose` | `x`, `y`, `z`, `roll`, `pitch`, `yaw`, `stance` (subset) | target posture (mm/degrees, limited), see [kinematik.md](kinematik.md). If it is not reachable, the nearest reachable one applies; the response then contains `"limited": true` and, in `msg`, what was limited |
| `pose_reset` | – | default posture |
| `walk` | `vx`, `vy`, `turn` (each −1…1) | drive command: forward/back, left/right, turn (+ = left). Only takes effect in IK mode (`stand` first). **No response**; without a new command for 0.6 s the robot stops – so send roughly every 100 ms while driving |
| `stop` | – | drive command to 0 (the robot still walks into the neutral position) |
| `stand` | – | stand up (from sitting; requires torque), switches to IK mode |
| `sit` | – | sit down (lowers raised front legs first) |
| `body` | `x`, `y`, `roll`, `pitch`, `yaw` (each −1…1) | body joystick: shift the body (±30 mm) and tilt it (±12°, yaw ±15°), feet stay planted. **No response**, send roughly every 100 ms like `walk`; without a new command the body springs back after 0.6 s |
| `gesture` | `name`: `circle`/`nod`/`shake`/`pushup`/`dance`/`stop` | play a gesture or end it smoothly (only while standing, front legs down), see [kinematik.md](kinematik.md#körper-bewegen-und-gesten) |
| `arms` | `up` (bool) | raise the front legs (weight to the rear, then raise; only when stationary) or lower them, see [kinematik.md](kinematik.md#vorderbeine) |
| `arm` | `lx`, `lz`, `rx`, `rz`, `reach`? (each −1…1) | control the front feet (l = leg 1, r = leg 4): x = swing about the coxa, + = inwards/forwards; z = up (+)/down; `reach` = both further out (+)/closer in. Only when raised; the foot goes as far as the leg reaches. **No response**, send roughly every 100 ms; without a command they return to the ready position after 0.6 s |
| `pad` | `lx`, `ly`, `rx`, `ry` (−1…1, y up +), `lt`, `rt` (0…1), `b` (button bits: a, b, x, y, lb, rb, view, menu, xbox, ls, rs, up, down, left, right, share, p1–p4) | gamepad in the browser, evaluated by the firmware like a Bluetooth controller. **No response**, send roughly every 50 ms; after 0.3 s without a command it is considered disconnected |
| `live` | `on` (bool, default `true`) | live view: this client receives `{"type":"live"}` at 20 Hz for 3 s (see below). **No response**; the UI renews this every 1.5 s while the 3D view is visible, `on: false` ends it immediately |
| `alive` | `on` (bool) | “Alive”: small random movements now and then while standing, see [kinematik.md](kinematik.md#lebendig); saved |
| `life` | `breathe`, `bored`, `lift`, `walk_fx`, `gaze` (bool), `sleep_min` (1–60), `lift_sens` (1 low, 2 medium, 3 high) – subset | “Lifelike behaviour”: breathing, boredom/falling asleep after `sleep_min`, flailing when picked up, walking with character, eyes follow the gaze, see [kinematik.md](kinematik.md#lebendig-wirken); saved |
| `wave` | – | right front leg waves (only when raised) |
| `gait` | `type` (`tripod`/`ripple`/`wave`/`climb`), `swing_s`, `step_height` (subset) | gait parameters, see [kinematik.md](kinematik.md#laufen); `type` also switches smoothly while walking (`motion.gait.switching` = transition in progress; from/to `climb` with a brief halt). With `climb`, `motion.gait.climb_height` reports the foot lift (mm) |
| `leveling` | `on` (bool) | levelling on/off, see [kinematik.md](kinematik.md#lageausgleich) |
| `imu_cal` | `level` (bool) | calibrate the attitude sensor (hold still for 2 s): gyro zero point, with `level` additionally current attitude = level. Progress/result in the telemetry |
| `imu_level_reset` | – | clear the zero point (level) |
| `imu_mount` | `x` (`vorne`/`links`/`hinten`/`rechts`), `upside_down` (bool) | mounting orientation of the attitude sensor (clears the zero point) |
| `servo_us` | `id`, `us` | raw mode: pulse width 500–2500 µs |
| `servo_deg` | `id`, `deg` | calibrated joint angle (limited to the limits) |
| `all_us` | `us` | all servos to one pulse width (assembly: 1500) |
| `all_deg` | `deg` | all joints to the same joint angle |
| `all_assembly` | – | all joints to the assembly pose (= default posture) |
| `slew` | `deg_per_s` | ramp speed for individual commands (10–1000 °/s, saved) |
| `cal_get` | `id`? | single servo → `cal`, without `id` → `calibration` |
| `cal_set` | `id`, `center_us`?, `us_per_deg`?, `min_deg`?, `max_deg`? | set values (validated) |
| `cal_center` | `id` | take the current position as the assembly pose (`center_us`) |
| `cal_ref` | `id`, `deg` | current position as the second reference angle → `us_per_deg` (at least 10° away from the assembly pose) |
| `cal_invert` | `id` | reverse the direction of rotation |
| `cal_save` | – | save the calibration to NVS |
| `cal_reload` | – | load the saved calibration (discard changes) |
| `cal_reset` | – | default values (not yet saved) |
| `eyes` | `color` (`#rrggbb`), `effect`, `ms` (100–120000, default 3000) or `reset: true` | set the eyes briefly (Behaviour page): colour and effect as on the LEDs page, does not override warnings (emergency stop, battery); `reset` = back to per-state. While the override is in effect, it appears in the telemetry under `eyes` |
| `client_log` | `text` (up to 100 characters) | message from the UI to the event log (warning), at most every 10 s, counted in between. **No response** |

All commands that change the calibration (`cal_set`, `cal_center`, `cal_ref`, `cal_invert`, `cal_reload`,
`cal_reset`) switch to manual mode: in IK mode, the new angle → pulse mapping would make all
affected joints jump. In manual mode the pulses stay where they are; when switching back
to IK mode, the posture is approached smoothly.

Example:

```json
{"cmd": "servo_deg", "id": 4, "deg": 30}
```

### Telemetry (ESP32 → all clients, every 250 ms)

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
             // only if a range is narrower than the scale: what limits it, per edge
             // [leg 1..6, joint 0 coxa/1 femur/2 tibia/3 foot not reachable, calibration limit °] or null
             "range_why": {"z": [null, [2, 1, 95]]},
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
  "wifi":  {"mode": "sta", "rssi": -58},              // + "ps": true in power-saving mode (Bluetooth on)
  "cam":   {"clients": 1, "awake": true, "fps": 12.4, "kb": 23, "kbps": 2300, "err": 0,
            "wake_ms": 640, "wakes": 3},
  "heap": 182000,
  "ws": 1,
  "ws_drop": 0,
  "ctl": {"hz": 50.0, "max_us": 2100}
}
```

`ws` = connected WebSocket clients, `ws_drop` = telemetry and live messages skipped since
start-up: if a client's send buffer is full (Wi-Fi can't keep up), the robot skips
these periodic messages for that client instead of waiting – so incoming commands (driving!)
don't get stuck. The UI shows the count for the last minute in the tooltip of the connection indicator.
If a client can't keep up at all for 10 s (connection dead, but not yet closed), the robot
disconnects it; the UI then reconnects.

`ctl`: control task (gait, IK, servos) – achieved frequency `hz` (target 50) and longest computation time of a
cycle in the last second `max_us`. `cam.bt_fs`: only with a Bluetooth controller connected, when the
camera is running at a lower resolution because of it (identifier as for `framesize` in `GET /api/camera`).

`bat.state`: `off` (monitoring switched off – `v` is then only an unchecked reading),
`usb` (no battery), `ok`, `warn`, `critical`. `servo.inhibit` contains the reason for a
lockout (e.g. “Battery empty”). `motion.unreachable` / `motion.clamped` list legs (0–5) that cannot reach their
foot point or that hit a joint limit, respectively. `motion.posture` (only in IK mode):
`sitting`, `standing_up`, `standing`, `sitting_down` – walking only happens in `standing`.
`motion.range`: for each quantity of the target posture, the range the legs can still reach with the other
values as set (calibration joint limits included) – the UI shows it as a band
below the slider and only lets the slider be dragged that far. Only recalculated when the target posture changes or every
2 s; if the target posture itself is not reachable, the fixed limits are given here.
`motion.gait`: `type` = selected,
`active` = running gait, `switching` = gait change while walking not yet complete (only if `true`), `swing` = bitmask of the legs in the air (bit 0 = leg 1),
`vx`/`vy` in mm/s, `wz` in °/s, `max_*` = maximum values with the current parameters (straight ahead,
sideways and turning on the spot, respectively, with reach limit), `nominal` = speed without
reach limit (if `max_speed` is significantly lower, the posture is restricting walking),
`cadence` = current step duration / configured one (1–2, slow = longer steps). `motion.arms`:
`down`, `shift_back`, `raise`, `up`, `lower`, `shift_forward`; `motion.gesture` only during a
gesture; `motion.body` while the body joystick is in effect, `offset_limited` if it or a front leg is at
the reach limit. `motion.alive`: setting `on`, `act` = running action (`look`, `shift`,
`tilt`, `lift`, `tap`, `shuffle`, on boredom/waking up `yawn`, `sigh`, `stretch`). `motion.life`: settings,
`mood` = `awake`, `bored` or `asleep`, `flail: true` while the robot is flailing. `led`: state of the eye LEDs (as in `GET /api/leds`). `pad`: source
(`ble`, `web` or empty), mode, speed in %, Bluetooth state (`off`, `idle`, `searching`, `pairing` with
`pair_left`, `connecting`, `connected` with `name`, `bat`), `in` = current controller state (only when active),
`msg`/`seq` = last message (new message = new `seq`). `log`: number of the newest entry in the
event log and of the newest warning (for the indicator in the menu). `motion.level`: levelling `on`, `active`
(currently in effect), current correction `roll`/`pitch` in degrees, `fault: true` = switched off because of a wrong
sensor orientation. `imu`: `pitch`/`roll` after subtracting the zero point `level`
([roll, pitch] of the board mounted at a tilt), `acc` in the body frame, `mount`/`flip` =
mounting orientation; `cal` (`running`/`ok`/`failed`) with `cal_pct` or `cal_msg` only during or shortly after
a calibration, respectively.

### Live view (ESP32 → clients with `live`, every 50 ms)

```json
{"type": "live", "t": 123456, "j": [0, 554, -338, …18…], "tq": true, "i": [-12, 8],
 "ik": true, "b": [0, 0, 52, 0, 0, 0], "s": 21, "v": [100, 0, 15]}
```

`j` = current target angles of the 18 joints (with ramp) in 0.1° (leg × 3 + coxa/femur/tibia, convention
as in [kinematik.md](kinematik.md)), `tq` = torque, `i` = measured attitude [roll, pitch] in 0.1° (only with
attitude sensor), `ik` = IK operation. Only in IK operation: `b` = body pose of the last IK [x, y, z in mm, roll,
pitch, yaw in degrees] (with levelling, body joystick, gestures), `s` = legs in the air (bitmask),
`v` = speed [vx, vy in mm/s, turn in °/s]. The UI uses this with forward kinematics to compute the
3D model.
