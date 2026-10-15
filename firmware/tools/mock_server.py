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
               "max_fps": 15, "rotation": 0, "xclk_mhz": 20, "xclk_active_mhz": 20, "bt_framesize": 6, "sharpness_ok": True,
               "image": {"brightness": 0, "contrast": 0, "saturation": 0, "sharpness": 0, "ae_level": 0, "gainceiling": 2,
                         "wb_mode": 0, "aec2": False, "lenc": True, "bpc": True}, "clients": 0, "port": 81, "sensor": "OV5640 (Mock)", "awake": False, "idle_sleep_s": 10,
               "zoom": 1.0, "zoom_hw": os.environ.get("MOCK_CAM_ZOOM_HW", "1") == "1"},
    "start": time.time(),
    "mode": "manual",
    "pose": {"x": 0, "y": 0, "z": 52, "roll": 0, "pitch": 0, "yaw": 0, "stance": 100},
    "walk": [0, 0, 0],
    "posture": "sitting",
    "posture_t": 0,
    "walk_t": 0,
    "gait": {"type": "tripod", "swing_s": 0.4, "step_height": 15},
    # Vorderbeine (wie arms.cpp): Ablauf aus (Zustand, Dauer); Gesten (wie gestures.cpp)
    "arms_seq": [("down", 0)], "arms_t": 0.0,
    "gesture": None, "gesture_t": 0.0,
    "body": [0.0] * 5, "body_t": 0.0,
    "alive": False,
    "life": {"breathe": True, "bored": True, "lift": True, "walk_fx": True, "gaze": True, "sleep_min": 5, "lift_sens": 2},
    "mood": "awake", "active_t": 0.0,
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


FW_VERSION = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "VERSION")).read().strip()


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
        # Empfang schwankt etwas (wie WiFi.RSSI() je Beacon)
        sta.update(ip="192.168.178.57", rssi=-52 + round(2 * math.sin(time.time() * 1.7)), channel=6, tries=2,
                   connect_ms=4200)
    if state["wifi_sta"] == "failed":
        sta.update(reason=state["wifi_reason"], retry_in=8)
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


ARMS_UP = [("shift_back", 1.2), ("raise", 1.0), ("up", 0)]
ARMS_DOWN = [("lower", 1.0), ("shift_forward", 1.2), ("down", 0)]
GESTURES = {"circle": 6.0, "nod": 3.4, "shake": 3.4, "pushup": 5.6, "dance": 6.4}


def arms():
    # aktueller Zustand im Ablauf; ohne Torque sofort unten
    if not state["torque"] or state["estop"] or state["mode"] != "pose":
        state["arms_seq"] = [("down", 0)]
    el = time.time() - state["arms_t"]
    for name, dur in state["arms_seq"]:
        if dur == 0 or el < dur:
            return name
        el -= dur
    return state["arms_seq"][-1][0]


def gesture():
    g = state["gesture"]
    if g and (time.time() - state["gesture_t"] > GESTURES[g] or posture() != "standing"):
        state["gesture"] = g = None
    return g


def z_max(stance):
    # grobe Reichweite: je breiter der Stand, desto weniger hoch
    return 150 - 0.5 * stance


def legs_text(legs):
    return ("Bein " if len(legs) == 1 else "Beine ") + ", ".join(str(l + 1) for l in legs)


def gait_status():
    g = dict(state["gait"])
    fresh = (time.time() - state["walk_t"] < 0.6 and state["torque"] and state["mode"] == "pose"
             and posture() == "standing" and arms() == "down")
    cmd = state["walk"] if fresh else [0, 0, 0]
    sigma = SWING_FRACTION[g["type"]]
    nominal = 50 / ((1 - sigma) * g["swing_s"] / sigma)
    # wie gait.cpp: hoch gestellt reicht das Bein nicht weit -> Tempo sinkt
    z = state["pose"]["z"]
    reach = 1.0 if z <= 85 else max(0.0, 1 - (z - 85) / 15)
    max_speed = nominal * reach
    walking = any(abs(v) > 1e-3 for v in cmd) and max_speed > 0
    # langsam = längere Schritte statt kürzerer (Schrittfrequenz passt sich an)
    mag = max((abs(v) for v in cmd), default=0)
    cadence = min(2.0, max(1.0, 0.5 / mag)) if walking and mag > 0 else 1.0
    cycle = g["swing_s"] * cadence / sigma
    g.update(active=g["type"], walking=walking, max_speed=round(max_speed), max_side=round(max_speed * 0.76),
             max_turn=round(max_speed / 181 * 57.3), nominal=round(nominal), cadence=round(cadence, 2),
             vx=0, vy=0, wz=0, swing=0)
    if walking and time.time() - state.get("gait_switch_t", 0) < 3:
        g["switching"] = True
    if walking:
        phase = (time.time() / cycle) % 1
        g["swing"] = sum(1 << i for i, s in enumerate(SWING_START[g["type"]]) if (phase - s) % 1 < sigma)
        g.update(vx=round(cmd[0] * max_speed), vy=round(cmd[1] * max_speed),
                 wz=round(cmd[2] * max_speed / 181 * 57.3))
    return g


ALIVE_ACTS = ["look", "shift", "lift", "tilt", "tap", "shuffle"]


def alive_tel(g):
    # wie alive.cpp (vereinfacht): im Stillstand alle 6 s eine Aktion von 2,5 s
    a = {"on": state["alive"]}
    t = time.time()
    if (state["alive"] and state["mode"] == "pose" and posture() == "standing" and arms() == "down"
            and not gesture() and not g["walking"] and t % 6 < 2.5):
        a["act"] = ALIVE_ACTS[int(t // 6) % len(ALIVE_ACTS)]
    return a


def pose_range():
    """Grobe Nachbildung von motion.range: tief unten wenig Spielraum, Verschieben/Neigen kostet Höhe"""
    p = state["pose"]
    k = min(1.0, max(0.15, (p["z"] - 30) / 22))
    z_lo = min(p["z"], 35 + (abs(p["x"]) + abs(p["y"])) / 2 + (abs(p["roll"]) + abs(p["pitch"])) * 1.5)
    z_hi = max(p["z"], min(100, z_max(p["stance"])))
    def sym(lim, cur):
        m = max(abs(cur), lim * k)
        return [round(-m, 1), round(m, 1)]
    return {"z": [round(z_lo, 1), round(z_hi, 1)], "stance": [min(p["stance"], 75 + max(0, 40 - p["z"])), 135],
            "x": sym(40, p["x"]), "y": sym(40, p["y"]), "roll": sym(20, p["roll"]), "pitch": sym(20, p["pitch"]),
            "yaw": [-25, 25]}


def eyes_tel():
    e = state.get("eyes")
    if not e or time.time() > e["until"]:
        state["eyes"] = None
        return {}
    return {"eyes": {"color": e["color"], "effect": e["effect"]}}


def life_tel(g):
    # wie life.cpp (vereinfacht, Zeiten in Sekunden statt Minuten): gelangweilt nach 40 %,
    # schläft (sitzt) nach der Einschlafzeit; Eingaben (Befehle) wecken ihn
    lf = state["life"]
    if not lf["bored"] or state["mode"] != "pose" or not state["torque"]:
        state["mood"] = "awake"
    elif state["mood"] != "asleep" and posture() == "standing" and not g["walking"]:
        idle = time.time() - state["active_t"]
        scale = float(os.environ.get("MOCK_LIFE_SCALE", "60"))  # Sekunden je "Minute"
        if idle >= lf["sleep_min"] * scale:
            state["mood"] = "asleep"
            state["posture"], state["posture_t"] = "sitting_down", time.time()
        elif idle >= 0.4 * lf["sleep_min"] * scale:
            state["mood"] = "bored"
    return {**lf, "mood": state["mood"]}


def motion_tel():
    g = gait_status()
    m = {"mode": state["mode"], "posture": posture(), "pose": state["pose"], "range": pose_range(), "gait": g,
         "level": level_tel(time.time()), "arms": arms(), "alive": alive_tel(g), "life": life_tel(g),
         "unreachable": [], "clamped": []}
    if gesture():
        m["gesture"] = gesture()
    body_on = time.time() - state["body_t"] < 0.6 and any(abs(v) > 1e-3 for v in state["body"])
    if body_on:
        m["body"] = True
        if max(abs(v) for v in state["body"]) > 0.9:
            m["offset_limited"] = True
    return m


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
        "cam": {"clients": 1, "err": 0, "fps": 12.4, "kb": 23, "kbps": 2300, "awake": True, "wake_ms": 640, "wakes": 3,
                **({"bt_fs": state["camera"]["bt_framesize"]} if PAD["enabled"] and state["camera"]["bt_framesize"] else {})},
        "motion": motion_tel(), "led": led_state(), "pad": pad_tel(), "log": log_tel(), **eyes_tel(),
        "wifi": wifi_tel(), "heap": 182000, "ws": 1, "ws_drop": 0,
    }


# --- Live-Ansicht (wie commands.cpp buildLive): Gelenkwinkel aus einer einfachen Gangsimulation
LEG = {"coxa": 41, "femur": 43, "tibia": 94}
MOUNTS = [(73.5, 38.5, 45), (0, 57, 90), (-73.5, 38.5, 135), (73.5, -38.5, -45), (0, -57, -90), (-73.5, -38.5, -135)]
LIVE = {"last": None, "odo": [0.0, 0.0, 0.0], "t": 0.0}


def leg_ik(leg, p):
    # wie kinematics.cpp legIk: Fuß im Bein-System -> (coxa, femur, tibia) in Grad, None = unerreichbar
    mx, my, ma = MOUNTS[leg]
    a = math.radians(-ma)
    x, y = p[0] - mx, p[1] - my
    fx, fy, fz = x * math.cos(a) - y * math.sin(a), x * math.sin(a) + y * math.cos(a), p[2]
    horiz = math.hypot(fx, fy)
    r = horiz - LEG["coxa"]
    d = math.hypot(r, fz)
    F, T = LEG["femur"], LEG["tibia"]
    if horiz < 1e-3 or d > F + T or d < abs(F - T):
        return None
    clamp = lambda v: max(-1.0, min(1.0, v))
    femur = math.atan2(fz, r) + math.acos(clamp((F * F + d * d - T * T) / (2 * F * d)))
    knee = math.acos(clamp((F * F + T * T - d * d) / (2 * F * T)))
    sign = -1 if math.sin(math.radians(ma)) > 0 else 1
    return (sign * math.degrees(math.atan2(fy, fx)), math.degrees(femur), math.degrees(knee) - 90)


def world_to_body(pose, w):
    cr, sr = math.cos(math.radians(pose[3])), math.sin(math.radians(pose[3]))
    cp, sp = math.cos(math.radians(-pose[4])), math.sin(math.radians(-pose[4]))
    cy, sy = math.cos(math.radians(pose[5])), math.sin(math.radians(pose[5]))
    dx, dy, dz = w[0] - pose[0], w[1] - pose[1], w[2] - pose[2]
    x1, y1 = cy * dx + sy * dy, -sy * dx + cy * dy
    x2, z2 = cp * x1 - sp * dz, sp * x1 + cp * dz
    return (x2, cr * y1 + sr * z2, -sr * y1 + cr * z2)


def live_msg():
    t = time.time()
    imu = imu_tel(t)
    msg = {"type": "live", "t": int((t - state["start"]) * 1000), "tq": state["torque"],
           "i": [round(imu["roll"] * 10), round(imu["pitch"] * 10)]}
    if state["mode"] != "pose":
        msg.update(ik=False, j=[round(deg_of(i) * 10) for i in range(18)])
        return msg
    ps = posture()
    pz = state["pose"]["z"]
    f = min(1.0, (t - state["posture_t"]) / 2.5)
    z = {"sitting": 22, "standing_up": 22 + (pz - 22) * f, "sitting_down": pz - (pz - 22) * f}.get(ps, pz)
    stance = state["pose"]["stance"] + (20 if ps == "sitting" else 0)
    pose = [state["pose"][k] for k in ("x", "y")] + [z] + [state["pose"][k] for k in ("roll", "pitch", "yaw")]
    g = gait_status()
    swing = g["swing"]
    sigma = SWING_FRACTION[g["type"]]
    cycle = g["swing_s"] * g["cadence"] / sigma
    phase = (t / cycle) % 1
    stance_s = (1 - sigma) * cycle
    j = []
    for leg, (mx, my, ma) in enumerate(MOUNTS):
        a = math.radians(ma)
        n = (mx + stance * math.cos(a), my + stance * math.sin(a))
        foot = [n[0], n[1], 0.0]
        if g["walking"]:
            # Fußgeschwindigkeit gegen den Körper (wie gait.cpp, ohne Rampen)
            wx = g["vx"] - math.radians(g["wz"]) * n[1]
            wy = g["vy"] + math.radians(g["wz"]) * n[0]
            p = (phase - SWING_START[g["type"]][leg]) % 1
            if p < sigma:  # Schwung: von hinten nach vorne, mit Hub
                u = p / sigma
                h = u * u * u * (10 + u * (-15 + 6 * u))
                k = (h - 0.5) * stance_s
                foot = [n[0] + wx * k, n[1] + wy * k, g["step_height"] * 16 * (u * (1 - u)) ** 2]
            else:  # Stand: von vorne nach hinten
                k = 0.5 * stance_s - (p - sigma) * cycle
                foot = [n[0] + wx * k, n[1] + wy * k, 0.0]
        ang = leg_ik(leg, world_to_body(pose, foot)) or (0.0, 30.0, -30.0)
        j += [round(v * 10) for v in ang]
    msg.update(ik=True, j=j, b=[round(v, 1) for v in pose], s=swing, v=[g["vx"], g["vy"], g["wz"]])
    return msg


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
    if c in ("walk", "body", "stand", "sit", "gesture", "arms", "wave", "pose") and any(
            abs(float(cmd.get(k, 0))) > 0.02 for k in ("vx", "vy", "turn", "x", "y", "roll", "pitch", "yaw")) or \
            c in ("stand", "sit", "gesture", "arms", "wave", "pose"):
        state["active_t"] = time.time()
        if state["mood"] == "asleep" and c != "sit":  # Bewegungswunsch weckt ihn
            state["mood"] = "awake"
            if c != "stand" and state["torque"] and posture() in ("sitting", "sitting_down"):
                state["posture"], state["posture_t"] = "standing_up", time.time()
        elif state["mood"] == "bored":
            state["mood"] = "awake"
    if c == "ping":
        return {"type": "pong"}
    if c == "client_log":  # wie die Firmware: ins Ereignisprotokoll, ohne Antwort
        ev("warn", "Oberfläche: " + str(cmd.get("text", ""))[:100])
        return None
    if c == "walk":  # wie die Firmware: ohne Antwort, nur im IK-Modus
        if state["mode"] != "pose":
            return None
        state["walk"] = [float(cmd.get(k, 0)) for k in ("vx", "vy", "turn")]
        state["walk_t"] = time.time()
        return None
    if c == "pad":  # Gamepad im Browser, ohne Antwort
        pad_input(cmd)
        return None
    if c == "body":  # Körper-Joystick, ohne Antwort
        state["body"] = [float(cmd.get(k, 0)) for k in ("x", "y", "roll", "pitch", "yaw")]
        state["body_t"] = time.time()
        return None
    if c == "arm":  # Vorderfüße, ohne Antwort
        return None
    out = {"type": "ack", "cmd": c, "ok": True}
    i = cmd.get("id")
    if c == "torque":
        if cmd.get("on") and state["estop"]:
            return {**out, "ok": False, "msg": "Not-Aus aktiv - erst freigeben"}
        if state["torque"] != bool(cmd.get("on")):
            ev("info", "Servos %s" % ("an" if cmd.get("on") else "aus"))
        state["torque"] = bool(cmd.get("on"))
    elif c == "estop":
        if not state["estop"]:
            ev("warn", "Not-Aus ausgelöst")
        state["estop"], state["torque"] = True, False
        out["msg"] = "Not-Aus ausgelöst"
    elif c == "estop_clear":
        if state["estop"]:
            ev("info", "Not-Aus freigegeben")
        state["estop"] = False
        out["msg"] = "Not-Aus freigegeben (Servos bleiben aus)"
    elif c == "mode":
        if cmd.get("mode") == "pose" and state["mode"] != "pose":
            # wie motion.cpp: beginnt in der Haltung, in der er ist (nach dem Start: sitzt)
            state["posture"] = "standing" if state.get("assembly") and state["torque"] else "sitting"
        elif cmd.get("mode") != "pose" and state["mode"] == "pose":
            state["assembly"] = posture() == "standing"  # Servos bleiben in der Standhaltung
        state["mode"] = cmd.get("mode", "manual")
        out["msg"] = "Körperhaltung (IK) aktiv" if state["mode"] == "pose" else "Manueller Modus"
    elif c == "pose":
        for k in ("x", "y", "z", "roll", "pitch", "yaw", "stance"):
            if k in cmd:
                state["pose"][k] = cmd[k]
        # wie motion.cpp: unerreichbare Haltung auf die nächste zulässige begrenzen
        zm = z_max(state["pose"]["stance"])
        if state["pose"]["z"] > zm:
            want = state["pose"]["z"]
            state["pose"]["z"] = round(zm)
            out["limited"] = True
            out["msg"] = (f"Haltung begrenzt: Höhe auf {round(zm)} mm (gewünscht {round(want)}) – "
                          f"{legs_text(range(6))} erreichen den Boden nicht")
    elif c == "arms":
        cur = arms()
        if cmd.get("up"):
            if cur in ("shift_back", "raise", "up"):
                pass
            elif cur != "down":
                return {**out, "ok": False, "msg": "Vorderbeine senken sich gerade"}
            elif not state["torque"] or posture() != "standing":
                return {**out, "ok": False, "msg": "Erst aufstehen"}
            elif gait_status()["walking"]:
                return {**out, "ok": False, "msg": "Erst anhalten"}
            else:
                state["gesture"] = None
                state["arms_seq"], state["arms_t"] = ARMS_UP, time.time()
            out["msg"] = "Vorderbeine heben"
        else:
            if cur == "shift_back":
                state["arms_seq"], state["arms_t"] = ARMS_DOWN[1:], time.time()
            elif cur in ("raise", "up"):
                state["arms_seq"], state["arms_t"] = ARMS_DOWN, time.time()
            out["msg"] = "Vorderbeine absetzen"
    elif c == "wave":
        if arms() != "up":
            return {**out, "ok": False, "msg": "Erst die Vorderbeine heben"}
    elif c == "gesture":
        name = cmd.get("name", "")
        if name == "stop":
            state["gesture"] = None
        elif name not in GESTURES:
            return {**out, "ok": False, "msg": "unbekannte Geste (circle, nod, shake, pushup, dance, stop)"}
        elif not state["torque"] or posture() != "standing":
            return {**out, "ok": False, "msg": "Erst aufstehen"}
        elif arms() != "down":
            return {**out, "ok": False, "msg": "Erst die Vorderbeine absetzen"}
        else:
            state["gesture"], state["gesture_t"] = name, time.time()
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
        state["gesture"] = None
        delay = 0.0
        if arms() in ("shift_back", "raise", "up"):  # wie motion.cpp: erst Vorderbeine absetzen
            state["arms_seq"], state["arms_t"] = ARMS_DOWN, time.time()
            delay = 2.2
        if posture() == "standing":
            state["posture"], state["posture_t"] = "sitting_down", time.time() + delay
        state["walk"] = [0, 0, 0]
        out["msg"] = "Setzt sich"
    elif c == "gait":
        if "type" in cmd:
            if cmd["type"] not in SWING_FRACTION:
                return {**out, "ok": False, "msg": "unbekannte Gangart (tripod, ripple, wave)"}
            if cmd["type"] != state["gait"]["type"] and gait_status()["walking"]:
                state["gait_switch_t"] = time.time()  # wie gait.cpp: fließender Übergang im Lauf
            state["gait"]["type"] = cmd["type"]
        if "swing_s" in cmd:
            state["gait"]["swing_s"] = max(0.3, min(1.2, float(cmd["swing_s"])))
        if "step_height" in cmd:
            state["gait"]["step_height"] = max(5, min(25, float(cmd["step_height"])))
    elif c == "alive":
        state["alive"] = bool(cmd.get("on"))
        out["msg"] = "Lebendig an" if state["alive"] else "Lebendig aus"
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
    elif c == "pose_reset":
        state["pose"] = {"x": 0, "y": 0, "z": 52, "roll": 0, "pitch": 0, "yaw": 0, "stance": 100}
        out["msg"] = "Haltung zurückgesetzt"
    elif c == "servo_us":
        state["us"][i] = max(500, min(2500, cmd["us"]))
        state["assembly"] = False
    elif c == "servo_deg":
        state["us"][i] = us_of(i, cmd["deg"])
        state["assembly"] = False
    elif c == "all_us":
        state["us"] = [float(cmd.get("us", 1500))] * 18
        state["assembly"] = False
    elif c == "all_deg":
        state["us"] = [us_of(k, cmd.get("deg", 0)) for k in range(18)]
        state["assembly"] = False
    elif c == "all_assembly":
        state["us"] = [cal["center_us"] for cal in state["cal"]]
        state["assembly"] = True  # Montagestellung = Standhaltung
    elif c == "slew":
        state["slew"] = float(cmd["deg_per_s"])
    elif c in ("cal_center", "cal_ref", "cal_invert", "cal_set", "cal_reload", "cal_reset") and state["mode"] == "pose":
        # wie die Firmware: Kalibrierung ändern schaltet in den Handbetrieb
        state["mode"] = "manual"
        return handle(cmd)
    elif c == "life":
        for k in ("breathe", "bored", "lift", "walk_fx", "gaze"):
            if k in cmd:
                state["life"][k] = bool(cmd[k])
        if "lift_sens" in cmd:
            if int(cmd["lift_sens"]) not in (1, 2, 3):
                return {**out, "ok": False, "msg": "lift_sens muss 1–3 sein"}
            state["life"]["lift_sens"] = int(cmd["lift_sens"])
        if "sleep_min" in cmd:
            m = float(cmd["sleep_min"])
            if not 1 <= m <= 60:
                return {**out, "ok": False, "msg": "sleep_min muss 1–60 sein"}
            state["life"]["sleep_min"] = round(m)
    elif c == "eyes":  # wie commands.cpp: Augen kurz vorgeben
        if cmd.get("reset"):
            state["eyes"] = None
        else:
            if cmd.get("effect", "solid") not in LED_EFFECTS:
                return {**out, "ok": False, "msg": "unbekannter Effekt"}
            ms = min(120000, max(100, float(cmd.get("ms", 3000))))
            state["eyes"] = {"color": cmd.get("color", "#ffffff"), "effect": cmd.get("effect", "solid"),
                             "until": time.time() + ms / 1000}
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

    live_until = [0.0]

    async def push_live():
        # wie web_server.cpp: 20 Hz, solange die Oberfläche "live" erneuert
        while not ws.closed:
            if time.time() < live_until[0]:
                await ws.send_str(json.dumps(live_msg()))
            await asyncio.sleep(0.05)

    task = asyncio.create_task(push())
    task_live = asyncio.create_task(push_live())
    async for msg in ws:
        if msg.type == WSMsgType.TEXT:
            data = json.loads(msg.data)
            if data.get("cmd") == "live":
                live_until[0] = time.time() + 3 if data.get("on", True) else 0
                continue
            reply = handle(data)
            if reply is not None:
                await ws.send_str(json.dumps(reply))
    task.cancel()
    task_live.cancel()
    return ws


async def status(_):
    return web.json_response({
        "fw": FW_VERSION, "git": "mock", "build": "mock", "uptime": int(time.time() - state["start"]),
        "chip": {"model": "ESP32-S3", "rev": 2, "cores": 2, "mhz": 240, "flash": 16777216},
        "mem": {"heap_free": 182000, "heap_min": 150000, "psram": 8388608, "psram_free": 6000000},
        "wifi": wifi_json(),
        "camera": state["camera"], "devices": {"pca1": True, "pca2": True, "imu": True, "camera": True},
        "bat": battery(), "i2c": i2c(),
    })


async def history_get(_):
    # 30 min Verlauf: Akku sinkt langsam, WLAN schwankt, Temperatur steigt mit der Kamera
    n = 180
    bat = [round(7.95 - 0.004 * i + 0.01 * math.sin(i / 7), 2) for i in range(n)]
    rssi = [None if 60 <= i < 64 else int(-60 + 6 * math.sin(i / 11) - (i % 5)) for i in range(n)]
    temp = [int(41 + 9 * (1 - math.exp(-i / 50))) for i in range(n)]
    heap = [int(176 - 4 * math.sin(i / 9)) for i in range(n)]
    return web.json_response({"period_s": 10, "age_ms": 3000, "bat": bat, "rssi": rssi, "temp": temp, "heap": heap})


# größter Zoom je Auflösung wie in der Firmware (OV5640: Sensor 2560×1920, höchstens 4×)
CAM_SIZES = {6: (320, 240, 2560, 1920), 10: (640, 480, 2560, 1920), 13: (1280, 720, 2560, 1440)}


def cam_zoom_max():
    if not state["camera"]["zoom_hw"]:
        return 4.0
    w, h, mw, mh = CAM_SIZES.get(state["camera"]["framesize"], (640, 480, 2560, 1920))
    return round(min(4.0, mw / w, mh / h), 2)


async def camera_get(_):
    cam = dict(state["camera"])
    cam["zoom_max"] = cam_zoom_max()
    cam["zoom"] = min(cam["zoom"], cam["zoom_max"])
    cam["framesizes"] = [{"id": 6, "name": "QVGA 320×240"}, {"id": 10, "name": "VGA 640×480"},
                         {"id": 13, "name": "HD 1280×720"}]
    return web.json_response(cam)


async def camera_post(request):
    body = await request.json()
    img = state["camera"]["image"]
    for k in list(body):
        if k in img:
            img[k] = body.pop(k)
    if "zoom" in body:
        try:
            body["zoom"] = round(min(4.0, max(1.0, float(body["zoom"]))) * 20) / 20
        except (TypeError, ValueError):
            body.pop("zoom")
    state["camera"].update(body)
    return await camera_get(request)


async def calibration_get(_):
    return web.json_response({"version": 3, "servos": state["cal"], "dirty": state["dirty"]})


async def calibration_post(request):
    data = await request.json()
    if len(data.get("servos", [])) != 18:
        return web.json_response({"ok": False, "msg": "erwarte 18 Servos"}, status=400)
    state["mode"] = "manual"  # wie die Firmware
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


# --- LEDs (wie status_leds.cpp / led_fx.cpp) ---
LED_FIXED = [("boot", "#666666", "solid"), ("wifi", "#0000ff", "blink"), ("bat_warn", "#ff6e00", "solid"),
             ("bat_low", "#ff0000", "breathe"), ("estop", "#ff0000", "blink")]
LED_DEFAULT_STATES = [("ap", "#0050ff", "breathe"), ("idle", "#009900", "solid"), ("torque", "#00ffff", "solid"),
                      ("stand", "#00ffff", "wink"), ("walk", "#008cff", "solid"), ("arms", "#aa00ff", "breathe"),
                      ("gesture", "#ff00a0", "rainbow")]
LED_EFFECTS = ["off", "solid", "breathe", "blink", "pulse", "wink", "alternate", "rainbow",
               "flicker", "strobe", "scan", "sleepy"]


def led_defaults():
    return {"mode": "auto", "brightness": 100, "board": "boot", "single": {"color": "#00ffff", "effect": "wink"},
            "states": {i: {"color": c, "effect": e} for i, c, e in LED_DEFAULT_STATES}}


LEDS = led_defaults()


def led_state():
    if state["estop"]:
        return "estop"
    if state["torque"]:
        g = gait_status()
        if arms() != "down":
            return "arms"
        if gesture():
            return "gesture"
        if g["walking"]:
            return "walk"
        return "stand" if state["mode"] == "pose" and posture() == "standing" else "torque"
    return "ap" if state["wifi_ap"] else "idle"


def leds_json():
    return {"mode": LEDS["mode"], "brightness": LEDS["brightness"], "board": LEDS["board"],
            "single": dict(LEDS["single"]),
            "states": [{"id": i, **LEDS["states"][i]} for i, _, _ in LED_DEFAULT_STATES],
            "fixed": [{"id": i, "color": c, "effect": e} for i, c, e in LED_FIXED], "state": led_state()}


def led_style(src, dst):
    if "color" in src:
        c = str(src["color"])
        if len(c) != 7 or c[0] != "#":
            return "color muss #rrggbb sein"
        dst["color"] = c.lower()
    if "effect" in src:
        if src["effect"] not in LED_EFFECTS:
            return "unbekannter Effekt"
        dst["effect"] = src["effect"]
    return None


async def leds_get(_):
    return web.json_response(leds_json())


async def leds_post(request):
    global LEDS
    body = await request.json()
    new = json.loads(json.dumps(LEDS))
    if body.get("reset"):
        new = led_defaults()
    if "mode" in body:
        if body["mode"] not in ("auto", "single", "off"):
            return web.json_response({"ok": False, "msg": "mode muss auto, single oder off sein"}, status=400)
        new["mode"] = body["mode"]
    if "brightness" in body:
        new["brightness"] = max(5, min(100, int(body["brightness"])))
    if "board" in body:
        if body["board"] not in ("boot", "mirror", "off"):
            return web.json_response({"ok": False, "msg": "board muss boot, mirror oder off sein"}, status=400)
        new["board"] = body["board"]
    if "single" in body:
        err = led_style(body["single"], new["single"])
        if err:
            return web.json_response({"ok": False, "msg": err}, status=400)
    if "state" in body:
        if body["state"] not in new["states"]:
            return web.json_response({"ok": False, "msg": "Zustand nicht einstellbar"}, status=400)
        err = led_style(body, new["states"][body["state"]])
        if err:
            return web.json_response({"ok": False, "msg": err}, status=400)
    LEDS = new
    return web.json_response({**leds_json(), "ok": True})


# --- Controller (vereinfacht wie pad_control.cpp / pad_map.cpp) ---
PAD_BUTTONS = ["a", "b", "x", "y", "lb", "rb", "view", "menu", "xbox", "ls", "rs", "up", "down", "left", "right",
               "share", "p1", "p2", "p3", "p4"]
PAD_ACTIONS = ["none", "stand_sit", "stop", "arms", "wave", "mode_next", "mode_walk", "mode_body", "mode_arms",
               "gait_prev", "gait_next", "height_up", "height_down", "speed_up", "speed_down", "alive", "leveling",
               "gesture_next", "circle", "nod", "shake", "pushup", "dance", "torque", "estop", "pose_reset"]
PAD_DEFAULT_MAP = ["stand_sit", "stop", "arms", "mode_next", "gait_prev", "gait_next", "alive", "gesture_next", "estop",
                   "leveling", "wave", "height_up", "height_down", "speed_down", "speed_up", "pose_reset",
                   "mode_walk", "mode_body", "mode_arms", "stop"]
PAD_MODES = ["walk", "body", "arms"]


def pad_defaults():
    return {"deadzone": 0.12, "expo": 0.35, "speed": 0.6, "invert_y": False, "swap_sticks": False,
            "map": dict(zip(PAD_BUTTONS, PAD_DEFAULT_MAP))}


PAD = {"cfg": pad_defaults(), "enabled": False, "addr": "", "ble": "off", "pair_until": 0.0, "web": None, "web_t": 0.0,
       "prev_b": 0, "mode": 0, "speed": 0.6, "msg": "", "seq": 0, "src": ""}
if os.environ.get("MOCK_PAD") == "ble":  # Controller schon gekoppelt und verbunden
    PAD.update(enabled=True, addr="98:7a:14:00:11:22", ble="connected")


def pad_msg(m):
    PAD["msg"] = m
    PAD["seq"] += 1


def pad_ble_state():
    if not PAD["enabled"]:
        return "off"
    if PAD["ble"] == "pairing" and time.time() > PAD["pair_until"]:
        # Mock: nach 3 s ist ein Controller gekoppelt
        PAD["ble"], PAD["addr"] = "connected", "98:7a:14:00:11:22"
        pad_msg("Controller gekoppelt")
    if PAD["ble"] == "off":
        PAD["ble"] = "searching" if PAD["addr"] else "idle"
    return PAD["ble"]


def pad_input(cmd):
    st = {k: float(cmd.get(k, 0)) for k in ("lx", "ly", "rx", "ry", "lt", "rt")}
    st["b"] = int(cmd.get("b", 0))
    PAD["web"], PAD["web_t"] = st, time.time()
    pressed = st["b"] & ~PAD["prev_b"]
    PAD["prev_b"] = st["b"]
    if not PAD["src"]:
        PAD["src"] = "web"
        return  # beim Verbinden gehaltene Tasten lösen nichts aus
    for i, name in enumerate(PAD_BUTTONS):
        if not pressed & (1 << i):
            continue
        act = PAD["cfg"]["map"][name]
        if act == "mode_next":
            PAD["mode"] = (PAD["mode"] + 1) % 3
            pad_msg("Modus: " + ["Laufen", "Körper", "Vorderbeine"][PAD["mode"]])
        elif act == "stand_sit":
            handle({"cmd": "sit" if posture() == "standing" else "stand"})
            pad_msg("Setzt sich" if posture() == "sitting_down" else "Steht auf")
        elif act == "speed_up" or act == "speed_down":
            PAD["speed"] = max(0.1, min(1.0, PAD["speed"] + (0.1 if act == "speed_up" else -0.1)))
            pad_msg("Tempo %d %%" % round(PAD["speed"] * 100))
        elif act == "estop":
            handle({"cmd": "estop"})
            pad_msg("Not-Aus – in der Oberfläche freigeben")
    if PAD_MODES[PAD["mode"]] == "walk" and state["mode"] == "pose":
        s = PAD["speed"]
        v = [st["ly"] * s, -st["lx"] * s, -st["rx"] * s]
        if any(abs(x) > 0.12 for x in v):
            handle({"cmd": "walk", "vx": v[0], "vy": v[1], "turn": v[2]})


def pad_tel():
    ble = pad_ble_state()
    if PAD["src"] == "web" and time.time() - PAD["web_t"] > 0.3:
        PAD["src"] = ""
    out = {"src": PAD["src"], "mode": PAD_MODES[PAD["mode"]], "speed": round(PAD["speed"] * 100), "ble": ble}
    if ble == "connected":
        out.update(name="Xbox Wireless Controller", bat=80)
    if ble == "pairing":
        out["pair_left"] = max(0, int(PAD["pair_until"] + 57 - time.time()))
    if PAD["src"] and PAD["web"]:
        out["in"] = PAD["web"]
    if PAD["seq"]:
        out.update(msg=PAD["msg"], seq=PAD["seq"])
    return out


def pad_json():
    return {"enabled": PAD["enabled"], "addr": PAD["addr"], **{k: PAD["cfg"][k] for k in ("deadzone", "expo", "speed", "invert_y", "swap_sticks")},
            "map": dict(PAD["cfg"]["map"]), "buttons": PAD_BUTTONS, "actions": PAD_ACTIONS}


async def pad_get(_):
    return web.json_response(pad_json())


async def pad_post(request):
    body = await request.json()
    cfg = json.loads(json.dumps(PAD["cfg"]))
    if body.get("reset"):
        cfg = pad_defaults()
    for k, lo, hi in (("deadzone", 0.02, 0.4), ("expo", 0.0, 1.0), ("speed", 0.1, 1.0)):
        if k in body:
            cfg[k] = max(lo, min(hi, float(body[k])))
    for k in ("invert_y", "swap_sticks"):
        if k in body:
            cfg[k] = bool(body[k])
    for b, a in body.get("map", {}).items():
        if b not in PAD_BUTTONS or a not in PAD_ACTIONS:
            return web.json_response({"ok": False, "msg": "unbekannte Taste oder Aktion: " + b}, status=400)
        cfg["map"][b] = a
    PAD["cfg"] = cfg
    PAD["speed"] = cfg["speed"]
    if "enabled" in body:
        PAD["enabled"] = bool(body["enabled"])
        if not PAD["enabled"]:
            PAD["ble"] = "off"
    if body.get("forget"):
        PAD["addr"] = ""
        PAD["ble"] = "idle" if PAD["enabled"] else "off"
    if body.get("pair"):
        PAD["enabled"] = True
        PAD["ble"], PAD["pair_until"] = "pairing", time.time() + 3
    return web.json_response({**pad_json(), "ok": True})


# --- Ereignisprotokoll (wie sys/evlog.cpp) ---
EVLOG = []


def ev(level, text):
    EVLOG.append({"seq": len(EVLOG) + 1, "ms": int((time.time() - state["start"]) * 1000), "lvl": level, "text": text})
    del EVLOG[:-48]


def log_tel():
    warn = [e["seq"] for e in EVLOG if e["lvl"] != "info"]
    return {"seq": EVLOG[-1]["seq"] if EVLOG else 0, "warn": warn[-1] if warn else 0}


async def log_get(request):
    since = int(request.query.get("since", 0))
    return web.json_response({"seq": EVLOG[-1]["seq"] if EVLOG else 0,
                              "uptime_ms": int((time.time() - state["start"]) * 1000),
                              "entries": [e for e in EVLOG if e["seq"] > since]})


async def ota_post(request):
    body = await request.read()
    if len(body) < 64 * 1024:
        return web.json_response({"ok": False, "msg": "Datei zu klein für eine Firmware"}, status=400)
    if body[0] != 0xE9:
        return web.json_response({"ok": False, "msg": "Keine ESP32-Firmware (.bin aus .pio/build/hexapod/firmware.bin verwenden)"}, status=400)
    ev("info", "Firmware-Update über die Weboberfläche gestartet (%d KB)" % (len(body) // 1024))
    ev("info", "Firmware-Update erfolgreich (%d KB) – Neustart" % (len(body) // 1024))
    return web.json_response({"ok": True, "msg": "Firmware übertragen – startet neu"})


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
                        "height_max": 25, "height_default": 15, "stride_max": 70},
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
    # Mock: wie nach einem Spannungseinbruch gestartet (zeigt die Warnung in der Oberfläche)
    ev("warn", "Start: Firmware %s (mock), Unterspannung (Brownout) – Akku schwach oder Servos ziehen die 5 V herunter"
       % FW_VERSION)
    ev("info", "WLAN verbunden: MockNet (192.168.1.50, -58 dBm)")
    app = web.Application()
    app.add_routes([
        web.get("/ws", ws_handler),
        web.get("/api/status", status),
        web.get("/api/camera", camera_get), web.post("/api/camera", camera_post),
        web.get("/api/calibration", calibration_get), web.post("/api/calibration", calibration_post),
        web.get("/api/wifi", wifi_get), web.post("/api/wifi", wifi_post),
        web.get("/api/wifi/scan", wifi_scan_get), web.post("/api/wifi/scan", wifi_scan_post),
        web.get("/api/battery", battery_get), web.post("/api/battery", battery_post),
        web.get("/api/leds", leds_get), web.post("/api/leds", leds_post),
        web.get("/api/pad", pad_get), web.post("/api/pad", pad_post),
        web.get("/api/log", log_get), web.post("/api/ota", ota_post), web.get("/api/history", history_get),
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
