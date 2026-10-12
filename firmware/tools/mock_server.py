"""Mock des Hexapod-Webservers zum Entwickeln der Oberfläche am PC.

Simuliert REST-API, WebSocket-Telemetrie und Servobefehle, sodass web/
ohne Roboter getestet werden kann (der Kamerastream fehlt).

    pip install aiohttp
    python tools/mock_server.py            # -> http://localhost:8080
"""

import asyncio
import json
import math
import os
import time

from aiohttp import WSMsgType, web

WEB_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "web")
DEFAULT_SLOPE = 2000 / 180

state = {
    "torque": False,
    "estop": False,
    "slew": 240.0,
    "dirty": False,
    "us": [1500.0] * 18,
    "cal": [],
    "camera": {"ok": True, "framesize": 10, "quality": 12, "vflip": False, "hmirror": False,
               "max_fps": 15, "rotation": 0, "xclk_mhz": 20, "xclk_active_mhz": 20, "clients": 0, "port": 81, "sensor": "OV5640 (Mock)", "awake": False, "idle_sleep_s": 10},
    "start": time.time(),
    "mode": "manual",
    "pose": {"x": 0, "y": 0, "z": 52, "roll": 0, "pitch": 0, "yaw": 0, "stance": 100},
    "walk": [0, 0, 0],
    "posture": "sitting",
    "posture_t": 0,
    "walk_t": 0,
    "gait": {"type": "tripod", "swing_s": 0.4, "step_height": 15},
    # Lagesensor (wie imu.cpp) und Lageausgleich (wie motion.cpp)
    "imu_mount": "vorne", "imu_flip": False, "imu_level": [0.0, 0.0],
    "imu_cal": None, "imu_cal_t": 0.0, "imu_cal_level": False, "imu_cal_msg": "",
    "leveling": False,
    # WLAN: startet im Access-Point-Modus (MOCK_WIFI=sta für Heimnetz-Betrieb)
    "wifi_ap": os.environ.get("MOCK_WIFI", "ap") == "ap",
    "wifi_sta": "idle" if os.environ.get("MOCK_WIFI", "ap") == "ap" else "connected",
    "wifi_ssid": "" if os.environ.get("MOCK_WIFI", "ap") == "ap" else "MockNet",
    "wifi_reason": "",
    "wifi_connect_at": 0.0,
    "wifi_pass": "",
    "bat_enabled": False,
    "bat_sleep": True,
}


def battery():
    if not state["bat_enabled"]:
        return {"v": 1.37, "pct": 0, "state": "off", "enabled": False, "deep_sleep": state["bat_sleep"]}
    return {"v": 7.62, "pct": 63, "state": "ok", "enabled": True, "deep_sleep": state["bat_sleep"]}


ASSEMBLY = [0.0, 55.4, -33.8]  # Montagestellung je Gelenktyp (robot_geometry.h)


def default_cal(i):
    # wie servo_map.h: linke Seite (Beine 1-3) dreht ohne Kalibrierung andersherum
    ref = ASSEMBLY[i % 3]
    direction = -1 if i < 9 else 1
    span = 60.0 if i % 3 == 0 else 70.0
    return {"id": i, "leg": i // 3, "joint": i % 3, "board": 1 if i < 9 else 0, "channel": i % 9,
            "ref_deg": ref, "center_us": 1500.0, "us_per_deg": round(direction * DEFAULT_SLOPE, 3),
            "min_deg": ref - span, "max_deg": ref + span}


def us_of(i, deg):
    c = state["cal"][i]
    d = max(c["min_deg"], min(c["max_deg"], deg))
    return c["center_us"] + (d - c["ref_deg"]) * c["us_per_deg"]


state["cal"] = [default_cal(i) for i in range(18)]
saved_cal = json.loads(json.dumps(state["cal"]))


def deg_of(i):
    c = state["cal"][i]
    return round(c["ref_deg"] + (state["us"][i] - c["center_us"]) / c["us_per_deg"], 1)


def i2c():
    return {"sda": 47, "scl": 21, "freq_khz": 400, "scanned": True, "age_s": 3, "duration_ms": 18,
            "devices": [{"addr": 0x40, "name": "PCA9685 #1 (Beine rechts)", "expected": True},
                        {"addr": 0x68, "name": "MPU6050", "expected": True}],
            "missing": [{"addr": 0x41, "name": "PCA9685 #2 (Beine links)"}]}


NETWORKS = [
    {"ssid": "FRITZ!Box 7590 XY", "rssi": -48, "channel": 6, "secure": True},
    {"ssid": "Nachbar-WLAN", "rssi": -71, "channel": 1, "secure": True},
    {"ssid": "Gastnetz", "rssi": -80, "channel": 11, "secure": False},
]


def wifi_update():
    # Verbindungsversuch nach 3 s auflösen: Passwort "falsch" -> Fehler
    if state["wifi_sta"] == "connecting" and time.time() >= state["wifi_connect_at"]:
        if state["wifi_pass"] == "falsch":
            state["wifi_sta"], state["wifi_reason"] = "failed", "Anmeldung abgelehnt – Passwort falsch?"
        else:
            state["wifi_sta"] = "connected"
            state["wifi_ap_off_at"] = time.time() + 120


def wifi_json():
    wifi_update()
    connected = state["wifi_sta"] == "connected"
    ap = {"active": state["wifi_ap"], "ssid": "Hexapod-MK3"}
    if state["wifi_ap"]:
        ap.update(ip="192.168.4.1", clients=1)
        if connected and state.get("wifi_ap_off_at"):
            ap["off_in_s"] = int(state["wifi_ap_off_at"] - time.time())
    sta = {"state": state["wifi_sta"], "ssid": state["wifi_ssid"]}
    if connected:
        sta.update(ip="192.168.178.57", rssi=-52)
    if state["wifi_sta"] == "failed":
        sta["reason"] = state["wifi_reason"]
    return {"mode": ("ap+sta" if connected else "ap") if state["wifi_ap"] else "sta",
            "ip": "192.168.178.57" if connected else "192.168.4.1", "host": "hexapod.local",
            "saved_ssid": state["wifi_ssid"], "ap": ap, "sta": sta}


SWING_FRACTION = {"tripod": 0.5, "ripple": 1 / 3, "wave": 1 / 6}
SWING_START = {  # wie gait.cpp: Beginn der Schwungphase je Bein
    "tripod": [0, 0.5, 0, 0.5, 0, 0.5],
    "ripple": [2 / 3, 1 / 3, 0, 1 / 6, 5 / 6, 0.5],
    "wave": [2 / 6, 1 / 6, 0, 5 / 6, 4 / 6, 3 / 6],
}


def posture():
    # wie motion.cpp: ohne Torque liegt der Roboter; Übergänge dauern ca. 2,5 s
    if not state["torque"] or state["estop"]:
        state["posture"] = "sitting"
    elif state["posture"] in ("standing_up", "sitting_down") and time.time() - state["posture_t"] > 2.5:
        state["posture"] = "standing" if state["posture"] == "standing_up" else "sitting"
    return state["posture"]


def gait_status():
    g = dict(state["gait"])
    fresh = (time.time() - state["walk_t"] < 0.6 and state["torque"] and state["mode"] == "pose"
             and posture() == "standing")
    cmd = state["walk"] if fresh else [0, 0, 0]
    sigma = SWING_FRACTION[g["type"]]
    cycle = g["swing_s"] / sigma
    max_speed = 50 / ((1 - sigma) * cycle)
    walking = any(abs(v) > 1e-3 for v in cmd)
    g.update(active=g["type"], walking=walking, max_speed=round(max_speed),
             max_turn=round(max_speed / 181 * 57.3), vx=0, vy=0, wz=0, swing=0)
    if walking:
        phase = (time.time() / cycle) % 1
        g["swing"] = sum(1 << i for i, s in enumerate(SWING_START[g["type"]]) if (phase - s) % 1 < sigma)
        g.update(vx=round(cmd[0] * max_speed), vy=round(cmd[1] * max_speed),
                 wz=round(cmd[2] * max_speed / 181 * 57.3))
    return g


def wifi_tel():
    # wie commands.cpp: rssi nur, wenn mit dem Heimnetz verbunden
    w = wifi_json()
    out = {"mode": w["mode"]}
    if "rssi" in w["sta"]:
        out["rssi"] = w["sta"]["rssi"]
    return out


def telemetry():
    t = time.time()
    return {
        "type": "telemetry", "uptime": int(t - state["start"]),
        "bat": battery(),
        "servo": {"torque": state["torque"], "active": 18 if state["torque"] else 0,
                  "estop": state["estop"], "inhibit": "", "slew": state["slew"],
                  "cal_dirty": state["dirty"], "us": [round(u) for u in state["us"]],
                  "deg": [deg_of(i) for i in range(18)], "boards": [True, True]},
        "imu": imu_tel(t),
        "ctl": {"hz": 50.0, "max_us": 1830},
        "cam": {"clients": 1, "err": 0, "fps": 12.4, "kb": 23, "kbps": 2300, "awake": True, "wake_ms": 640, "wakes": 3},
        "motion": {"mode": state["mode"], "posture": posture(), "pose": state["pose"], "gait": gait_status(),
                   "level": level_tel(t), "unreachable": [], "clamped": []},
        "wifi": wifi_tel(), "heap": 182000, "ws": 1,
    }


def imu_tel(t):
    # Lage schwingt etwas; der Nullpunkt wird abgezogen
    lr, lp = state["imu_level"]
    out = {"ok": True, "who": 0x68, "errors": 0, "pitch": round(1.5 + 3 * math.sin(t / 3) - lp, 1),
           "roll": round(-2.0 + 5 * math.sin(t / 2) - lr, 1), "acc": [0.05, -0.09, 0.99], "hz": 50,
           "temp": 28.4, "mount": state["imu_mount"], "flip": state["imu_flip"],
           "level": [round(lr, 1), round(lp, 1)], "bias_src": "beim Start gemessen"}
    cal = state["imu_cal"]
    if cal == "running":
        pct = int((t - state["imu_cal_t"]) / 2.0 * 100)
        if pct >= 100:
            if state["imu_cal_level"]:
                state["imu_level"] = [-2.0, 1.5]
                state["imu_cal_msg"] = "Nullpunkt gesetzt (Sensor um Roll -2.0°, Nick +1.5° geneigt)"
            else:
                state["imu_cal_msg"] = "Gyro-Nullpunkt neu gemessen"
            state["imu_cal"], state["imu_cal_t"] = "ok", t
        else:
            out["cal"], out["cal_pct"] = "running", pct
    if state["imu_cal"] == "ok":
        if t - state["imu_cal_t"] > 10:
            state["imu_cal"] = None
        else:
            out["cal"], out["cal_msg"] = "ok", state["imu_cal_msg"]
    return out


def level_tel(t):
    active = state["leveling"] and state["mode"] == "pose" and posture() == "standing"
    return {"on": state["leveling"], "active": active,
            "roll": round(2.0 * math.sin(t / 4), 1) if active else 0.0,
            "pitch": round(-1.0 * math.sin(t / 5), 1) if active else 0.0}


def handle(cmd):
    c = cmd.get("cmd", "")
    if c == "ping":
        return {"type": "pong"}
    if c == "walk":  # wie die Firmware: ohne Antwort
        state["mode"] = "pose"
        state["walk"] = [float(cmd.get(k, 0)) for k in ("vx", "vy", "turn")]
        state["walk_t"] = time.time()
        return None
    out = {"type": "ack", "cmd": c, "ok": True}
    i = cmd.get("id")
    if c == "torque":
        if cmd.get("on") and state["estop"]:
            return {**out, "ok": False, "msg": "Not-Aus aktiv - erst freigeben"}
        state["torque"] = bool(cmd.get("on"))
    elif c == "estop":
        state["estop"], state["torque"] = True, False
        out["msg"] = "Not-Aus ausgelöst"
    elif c == "estop_clear":
        state["estop"] = False
        out["msg"] = "Not-Aus freigegeben (Servos bleiben aus)"
    elif c == "mode":
        if cmd.get("mode") == "pose" and state["mode"] != "pose":
            state["posture"] = "standing"
        state["mode"] = cmd.get("mode", "manual")
        out["msg"] = "Körperhaltung (IK) aktiv" if state["mode"] == "pose" else "Manueller Modus"
    elif c == "pose":
        for k in ("x", "y", "z", "roll", "pitch", "yaw", "stance"):
            if k in cmd:
                state["pose"][k] = cmd[k]
    elif c == "stop":
        state["walk"] = [0, 0, 0]
    elif c == "stand":
        if not state["torque"]:
            return {**out, "ok": False, "msg": "Erst Torque einschalten"}
        if state["mode"] != "pose":
            state["mode"], state["posture"] = "pose", "sitting"
        if posture() == "sitting":
            state["posture"], state["posture_t"] = "standing_up", time.time()
        out["msg"] = "Steht auf"
    elif c == "sit":
        state["mode"] = "pose"
        if posture() == "standing":
            state["posture"], state["posture_t"] = "sitting_down", time.time()
        state["walk"] = [0, 0, 0]
        out["msg"] = "Setzt sich"
    elif c == "gait":
        if "type" in cmd:
            if cmd["type"] not in SWING_FRACTION:
                return {**out, "ok": False, "msg": "unbekannte Gangart (tripod, ripple, wave)"}
            state["gait"]["type"] = cmd["type"]
        if "swing_s" in cmd:
            state["gait"]["swing_s"] = max(0.3, min(1.2, float(cmd["swing_s"])))
    elif c == "leveling":
        state["leveling"] = bool(cmd.get("on"))
        out["msg"] = "Lageausgleich an" if state["leveling"] else "Lageausgleich aus"
    elif c == "imu_cal":
        state["imu_cal"], state["imu_cal_t"] = "running", time.time()
        state["imu_cal_level"] = bool(cmd.get("level"))
        out["msg"] = "Nullpunkt wird gemessen – Roboter 2 s ruhig und waagerecht halten"
    elif c == "imu_level_reset":
        state["imu_level"] = [0.0, 0.0]
        out["msg"] = "Nullpunkt zurückgesetzt"
    elif c == "imu_mount":
        if cmd.get("x") not in ("vorne", "links", "hinten", "rechts"):
            return {**out, "ok": False, "msg": "x muss vorne, links, hinten oder rechts sein"}
        state["imu_mount"], state["imu_flip"] = cmd["x"], bool(cmd.get("upside_down"))
        state["imu_level"] = [0.0, 0.0]
        out["msg"] = "Einbaulage gespeichert – Nullpunkt neu setzen"
        if "step_height" in cmd:
            state["gait"]["step_height"] = max(5, min(25, float(cmd["step_height"])))
    elif c == "pose_reset":
        state["pose"] = {"x": 0, "y": 0, "z": 52, "roll": 0, "pitch": 0, "yaw": 0, "stance": 100}
        out["msg"] = "Haltung zurückgesetzt"
    elif c == "servo_us":
        state["us"][i] = max(500, min(2500, cmd["us"]))
    elif c == "servo_deg":
        state["us"][i] = us_of(i, cmd["deg"])
    elif c == "all_us":
        state["us"] = [float(cmd.get("us", 1500))] * 18
    elif c == "all_deg":
        state["us"] = [us_of(k, cmd.get("deg", 0)) for k in range(18)]
    elif c == "all_assembly":
        state["us"] = [cal["center_us"] for cal in state["cal"]]
    elif c == "slew":
        state["slew"] = float(cmd["deg_per_s"])
    elif c == "cal_get":
        if i is None:
            out["calibration"] = {"version": 3, "servos": state["cal"], "dirty": state["dirty"]}
        else:
            out["cal"] = state["cal"][i]
    elif c == "cal_center":
        state["cal"][i]["center_us"] = state["us"][i]
        state["dirty"] = True
        out.update(msg="Montagestellung übernommen", cal=state["cal"][i])
    elif c == "cal_ref":
        delta = state["us"][i] - state["cal"][i]["center_us"]
        d_deg = cmd["deg"] - state["cal"][i]["ref_deg"]
        if abs(d_deg) < 10:
            out.update(ok=False, msg="Referenzwinkel muss mindestens 10 Grad von der Montagestellung entfernt sein")
        elif abs(delta) < 40:
            out.update(ok=False, msg="Servo steht noch zu nah an der Montagestellung")
        else:
            state["cal"][i]["us_per_deg"] = round(delta / d_deg, 3)
            state["dirty"] = True
            out["msg"] = "Referenzwinkel übernommen"
        out["cal"] = state["cal"][i]
    elif c == "cal_invert":
        state["cal"][i]["us_per_deg"] *= -1
        state["dirty"] = True
        out.update(msg="Drehrichtung umgekehrt", cal=state["cal"][i])
    elif c == "cal_set":
        for k in ("center_us", "us_per_deg", "min_deg", "max_deg"):
            if k in cmd:
                state["cal"][i][k] = cmd[k]
        state["dirty"] = True
        out["cal"] = state["cal"][i]
    elif c == "cal_save":
        saved_cal[:] = json.loads(json.dumps(state["cal"]))
        state["dirty"] = False
        out["msg"] = "Kalibrierung gespeichert"
    elif c == "cal_reload":
        state["cal"] = json.loads(json.dumps(saved_cal))
        state["dirty"] = False
        out["msg"] = "Gespeicherte Kalibrierung geladen"
    elif c == "cal_reset":
        state["cal"] = [default_cal(k) for k in range(18)]
        state["dirty"] = True
        out["msg"] = "Standardwerte gesetzt (noch nicht gespeichert)"
    else:
        out.update(ok=False, msg="unbekannter Befehl: " + c)
    return out


async def ws_handler(request):
    ws = web.WebSocketResponse()
    await ws.prepare(request)

    async def push():
        while not ws.closed:
            await ws.send_str(json.dumps(telemetry()))
            await asyncio.sleep(0.25)

    task = asyncio.create_task(push())
    async for msg in ws:
        if msg.type == WSMsgType.TEXT:
            reply = handle(json.loads(msg.data))
            if reply is not None:
                await ws.send_str(json.dumps(reply))
    task.cancel()
    return ws


async def status(_):
    return web.json_response({
        "fw": "0.1.0-mock", "build": "mock", "uptime": int(time.time() - state["start"]),
        "chip": {"model": "ESP32-S3", "rev": 2, "cores": 2, "mhz": 240, "flash": 16777216},
        "mem": {"heap_free": 182000, "heap_min": 150000, "psram": 8388608, "psram_free": 6000000},
        "wifi": wifi_json(),
        "camera": state["camera"], "devices": {"pca1": True, "pca2": True, "imu": True, "camera": True},
        "bat": battery(), "i2c": i2c(),
    })


async def camera_get(_):
    cam = dict(state["camera"])
    cam["framesizes"] = [{"id": 6, "name": "QVGA 320×240"}, {"id": 10, "name": "VGA 640×480"},
                         {"id": 13, "name": "HD 1280×720"}]
    return web.json_response(cam)


async def camera_post(request):
    state["camera"].update(await request.json())
    return await camera_get(request)


async def calibration_get(_):
    return web.json_response({"version": 3, "servos": state["cal"], "dirty": state["dirty"]})


async def calibration_post(request):
    data = await request.json()
    if len(data.get("servos", [])) != 18:
        return web.json_response({"ok": False, "msg": "erwarte 18 Servos"}, status=400)
    state["cal"] = sorted(data["servos"], key=lambda c: c["id"])
    saved_cal[:] = json.loads(json.dumps(state["cal"]))
    state["dirty"] = False
    return web.json_response({"ok": True, "msg": "Kalibrierung importiert und gespeichert"})


async def ok(_):
    return web.json_response({"ok": True, "msg": "OK (Mock)"})


async def wifi_get(_):
    return web.json_response(wifi_json())


async def wifi_post(request):
    data = await request.json()
    if data.get("clear"):
        state["wifi_ssid"] = ""
        return web.json_response({"ok": True, "msg": "WLAN-Daten gelöscht"})
    ssid, pw = data.get("ssid", ""), data.get("pass", "")
    if not ssid or (pw and len(pw) < 8 and pw != "falsch"):
        return web.json_response({"ok": False, "msg": "Passwort muss leer oder 8–64 Zeichen lang sein"}, status=400)
    state["wifi_ssid"], state["wifi_pass"] = ssid, pw
    if data.get("connect") and state["wifi_ap"]:
        state["wifi_sta"], state["wifi_connect_at"] = "connecting", time.time() + 3
        return web.json_response({"ok": True, "msg": f"Gespeichert – verbinde mit „{ssid}“ …"})
    return web.json_response({"ok": True, "msg": "Gespeichert – wird nach einem Neustart verwendet"})


async def wifi_scan_post(_):
    state["wifi_scan_until"] = time.time() + 2
    return web.json_response({"ok": True, "msg": "Suche gestartet"})


async def wifi_scan_get(_):
    running = time.time() < state.get("wifi_scan_until", 0)
    return web.json_response({"running": running, "age_s": 0, "networks": [] if running else NETWORKS})


async def battery_get(_):
    return web.json_response(battery())


async def battery_post(request):
    data = await request.json()
    state["bat_enabled"] = data.get("enabled", state["bat_enabled"])
    state["bat_sleep"] = data.get("deep_sleep", state["bat_sleep"])
    return web.json_response({**battery(), "ok": True, "msg": "Akkuüberwachung " + ("an" if state["bat_enabled"] else "aus")})


async def kinematics(_):
    return web.json_response({
        "placeholder": False, "leg": {"coxa": 41, "femur": 43, "tibia": 94},
        "mounts": [{"x": 73.5, "y": 38.5, "angle": 45}, {"x": 0, "y": 57, "angle": 90},
                   {"x": -73.5, "y": 38.5, "angle": 135}, {"x": 73.5, "y": -38.5, "angle": -45},
                   {"x": 0, "y": -57, "angle": -90}, {"x": -73.5, "y": -38.5, "angle": -135}],
        "limits": {"height_min": 35, "height_max": 100, "height_default": 52,
                   "stance_min": 75, "stance_max": 135, "stance_default": 100,
                   "shift_max": 40, "tilt_max": 20, "yaw_max": 25},
        "gait_limits": {"swing_min": 0.3, "swing_max": 1.2, "swing_default": 0.4, "height_min": 5,
                        "height_max": 25, "height_default": 15, "stride_max": 50},
        "knee_min": 25})


async def i2c_get(_):
    return web.json_response(i2c())


async def i2c_scan(_):
    await asyncio.sleep(0.05)
    return web.json_response({**i2c(), "ok": True})


async def estop(_):
    handle({"cmd": "estop"})
    return web.json_response({"ok": True, "msg": "Not-Aus ausgelöst"})


async def static(request):
    name = request.match_info.get("name") or "index.html"
    path = os.path.join(WEB_DIR, name)
    if not os.path.isfile(path):
        raise web.HTTPNotFound()
    return web.FileResponse(path, headers={"Cache-Control": "no-cache"})


def main():
    app = web.Application()
    app.add_routes([
        web.get("/ws", ws_handler),
        web.get("/api/status", status),
        web.get("/api/camera", camera_get), web.post("/api/camera", camera_post),
        web.get("/api/calibration", calibration_get), web.post("/api/calibration", calibration_post),
        web.get("/api/wifi", wifi_get), web.post("/api/wifi", wifi_post),
        web.get("/api/wifi/scan", wifi_scan_get), web.post("/api/wifi/scan", wifi_scan_post),
        web.get("/api/battery", battery_get), web.post("/api/battery", battery_post),
        web.get("/api/kinematics", kinematics),
        web.get("/api/i2c", i2c_get), web.post("/api/i2c/scan", i2c_scan),
        web.post("/api/estop", estop), web.post("/api/reboot", ok),
        web.get("/", static), web.get("/{name}", static),
    ])
    port = int(os.environ.get("PORT", "8080"))
    print(f"Mock-Server: http://localhost:{port}")
    web.run_app(app, port=port, print=None)


if __name__ == "__main__":
    main()
