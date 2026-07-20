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

def history():
    series = {}
    for k, base in [("co2", 620), ("pm1", 3), ("pm25", 4), ("pm4", 5), ("pm10", 7),
                    ("voc", 87), ("nox", 12), ("temp", 22), ("hum", 46)]:
        series[k] = [None if i < 4 else round(base * (1 + 0.2 * math.sin(i / 5) + random.uniform(-.05, .05)), 1)
                     for i in range(48)]
    return {"interval": 1800, "points": 48, "series": series}

class H(SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith('/api/data'):    return self.j(data())
        if self.path.startswith('/api/history'): return self.j(history())
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
