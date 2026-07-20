#!/usr/bin/env python3
"""Dev server: serves dev/*.html plus fake /api endpoints on :8000."""
import json, math, random, time
from http.server import HTTPServer, SimpleHTTPRequestHandler

STATE = 'online'   # change by hand to test warmup/calibrating/fault

def data():
    t = time.time()
    return {"state": STATE, "ageMs": 7_200_000,
            "co2": int(620 + 40 * math.sin(t / 60)), "temp": 22.4, "hum": 46.0,
            "pm1": 3.1, "pm25": 4.2, "pm4": 5.0, "pm10": 6.8,
            "voc": 87, "nox": 12, "faultFlags": 16 if STATE == 'fault' else 0}

def settings():
    return {"altitude": 0, "usePressure": False, "pressure": 1013, "tempOffset": 0.0, "tempSlope": 0.0,
            "vocPersist": True, "asc": True, "frcTarget": 420,
            "vocIdxOff": 100, "vocLearnOff": 12, "vocLearnGain": 12, "vocGateMax": 180, "vocStdInit": 50, "vocGain": 230,
            "noxIdxOff": 1, "noxLearnOff": 12, "noxGateMax": 720, "noxGain": 230}

def device():
    return {"product": "SEN66", "serial": "0F3A-77C2-19", "fw": "2.3",
            "flags": {"fan": False, "rht": False, "gas": False, "co2a": False, "co2b": False, "pm": False, "speed": False},
            "rssi": -58, "ip": "192.168.1.42", "heap": 191488, "uptimeS": 534720,
            "lastFan": 1752305400, "lastHeat": 1749229920}

def history():
    series = {}
    for k, base in [("co2", 620), ("pm1", 3), ("pm25", 4), ("pm4", 5), ("pm10", 7),
                    ("voc", 87), ("nox", 12), ("temp", 22), ("hum", 46)]:
        series[k] = [None if i < 4 else round(base * (1 + 0.2 * math.sin(i / 5) + random.uniform(-.05, .05)), 1)
                     for i in range(48)]
    return {"interval": 1800, "points": 48, "series": series}

class H(SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith('/api/data'):     return self.j(data())
        if self.path.startswith('/api/history'):  return self.j(history())
        if self.path.startswith('/api/settings'): return self.j(settings())
        if self.path.startswith('/api/device'):   return self.j(device())
        if self.path == '/':      self.path = '/dev/dashboard.html'
        if self.path == '/admin': self.path = '/dev/admin.html'
        return super().do_GET()
    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length', 0)))
        time.sleep(0.3)
        self.j({"ok": True})
    def j(self, obj):
        b = json.dumps(obj).encode()
        self.send_response(200); self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', len(b)); self.end_headers(); self.wfile.write(b)

HTTPServer(('', 8000), H).serve_forever()
