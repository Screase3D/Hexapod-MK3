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
               "max_fps": 15, "clients": 0, "port": 81, "sensor": "OV2640 (Mock)"},
    "start": time.time(),
    "bat_enabled": False,
    "bat_sleep": True,
}


def battery():
    if not state["bat_enabled"]:
        return {"v": 1.37, "pct": 0, "state": "off", "enabled": False, "deep_sleep": state["bat_sleep"]}
    return {"v": 7.62, "pct": 63, "state": "ok", "enabled": True, "deep_sleep": state["bat_sleep"]}


def default_cal(i):
    return {"id": i, "leg": i // 3, "joint": i % 3, "board": 0 if i < 9 else 1, "channel": i % 9,
            "center_us": 1500.0, "us_per_deg": round(DEFAULT_SLOPE, 3),
            "min_deg": -60.0 if i % 3 == 0 else -80.0, "max_deg": 60.0 if i % 3 == 0 else 80.0}


state["cal"] = [default_cal(i) for i in range(18)]
saved_cal = json.loads(json.dumps(state["cal"]))


def deg_of(i):
    c = state["cal"][i]
    return round((state["us"][i] - c["center_us"]) / c["us_per_deg"], 1)


def telemetry():
    t = time.time()
    return {
        "type": "telemetry", "uptime": int(t - state["start"]),
        "bat": battery(),
        "servo": {"torque": state["torque"], "active": 18 if state["torque"] else 0,
                  "estop": state["estop"], "inhibit": "", "slew": state["slew"],
                  "cal_dirty": state["dirty"], "us": [round(u) for u in state["us"]],
                  "deg": [deg_of(i) for i in range(18)], "boards": [True, True]},
        "imu": {"ok": True, "pitch": round(3 * math.sin(t / 3), 1), "roll": round(5 * math.sin(t / 2), 1)},
        "wifi": {"mode": "sta", "rssi": -58}, "heap": 182000, "ws": 1,
    }


def handle(cmd):
    c = cmd.get("cmd", "")
    if c == "ping":
        return {"type": "pong"}
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
    elif c == "servo_us":
        state["us"][i] = max(500, min(2500, cmd["us"]))
    elif c == "servo_deg":
        cal = state["cal"][i]
        d = max(cal["min_deg"], min(cal["max_deg"], cmd["deg"]))
        state["us"][i] = cal["center_us"] + d * cal["us_per_deg"]
    elif c == "all_us":
        state["us"] = [float(cmd.get("us", 1500))] * 18
    elif c == "all_deg":
        state["us"] = [cal["center_us"] for cal in state["cal"]]
    elif c == "slew":
        state["slew"] = float(cmd["deg_per_s"])
    elif c == "cal_get":
        if i is None:
            out["calibration"] = {"version": 1, "servos": state["cal"], "dirty": state["dirty"]}
        else:
            out["cal"] = state["cal"][i]
    elif c == "cal_zero":
        state["cal"][i]["center_us"] = state["us"][i]
        state["dirty"] = True
        out.update(msg="Neutralstellung übernommen", cal=state["cal"][i])
    elif c == "cal_ref":
        delta = state["us"][i] - state["cal"][i]["center_us"]
        if abs(delta) < 40:
            out.update(ok=False, msg="Servo steht noch zu nah an 0 Grad")
        else:
            state["cal"][i]["us_per_deg"] = round(delta / cmd["deg"], 3)
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
            await ws.send_str(json.dumps(handle(json.loads(msg.data))))
    task.cancel()
    return ws


async def status(_):
    return web.json_response({
        "fw": "0.1.0-mock", "build": "mock", "uptime": int(time.time() - state["start"]),
        "chip": {"model": "ESP32-S3", "rev": 2, "cores": 2, "mhz": 240, "flash": 16777216},
        "mem": {"heap_free": 182000, "heap_min": 150000, "psram": 8388608, "psram_free": 6000000},
        "wifi": {"mode": "sta", "ip": "127.0.0.1", "host": "hexapod.local", "ssid": "MockNet",
                 "rssi": -58, "connected": True},
        "camera": state["camera"], "devices": {"pca1": True, "pca2": True, "imu": True, "camera": True},
        "bat": battery(),
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
    return web.json_response({"version": 1, "servos": state["cal"], "dirty": state["dirty"]})


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
    return web.json_response({"mode": "sta", "ip": "127.0.0.1", "ssid": "MockNet", "saved_ssid": "MockNet"})


async def battery_get(_):
    return web.json_response(battery())


async def battery_post(request):
    data = await request.json()
    state["bat_enabled"] = data.get("enabled", state["bat_enabled"])
    state["bat_sleep"] = data.get("deep_sleep", state["bat_sleep"])
    return web.json_response({**battery(), "ok": True, "msg": "Akkuüberwachung " + ("an" if state["bat_enabled"] else "aus")})


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
        web.get("/api/wifi", wifi_get), web.post("/api/wifi", ok),
        web.get("/api/battery", battery_get), web.post("/api/battery", battery_post),
        web.post("/api/estop", estop), web.post("/api/reboot", ok),
        web.get("/", static), web.get("/{name}", static),
    ])
    port = int(os.environ.get("PORT", "8080"))
    print(f"Mock-Server: http://localhost:{port}")
    web.run_app(app, port=port, print=None)


if __name__ == "__main__":
    main()
