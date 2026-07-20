# Web Dashboard locale (ESP32 + SEN66) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Sostituire Blynk con una dashboard web servita dall'ESP32 (design "Air Dashboard v2") più pannello `/admin` completo per il SEN66.

**Architecture:** Sketch Arduino a 3 file (`airsensor.ino` + 2 header PROGMEM con le pagine). Le pagine sono sviluppate come HTML standalone in `dev/` (testabili in browser con un mock server Python) e poi incapsulate nei `.h` da uno script. API JSON hand-rolled (niente ArduinoJson); POST in `application/x-www-form-urlencoded` (parsing nativo di `WebServer`).

**Tech Stack:** ESP32 Arduino core (`WebServer.h`, `ESPmDNS.h`, `Preferences.h`), SensirionI2cSen66, HTML/CSS/JS vanilla, Python 3 (solo dev).

**Riferimenti nel repo:**
- Spec: `docs/superpowers/specs/2026-07-20-web-dashboard-design.md`
- Design da replicare: `docs/design/air-dashboard-v2.dc.html` (contiene markup, palette, e le funzioni JS `co2Band`, `pm25Band`, `idxBand`, `chart`, `gaugeEl`, la mappa `states` — vanno portate in JS vanilla identiche nei valori)

## Global Constraints

- Tutto il firmware in inglese (commenti inclusi), coerente con il codice esistente.
- Palette e tipografia ESATTE del design: bg `#0d1117`, card `#141b24`, bordo `#1f2937`, testo `#e7ecf2`, muted `#7a8595`, verde `#4fc98f`, lime `#a8c957`, giallo `#e0b34f`, arancio `#e08a4f`, rosso `#e06e5b`, blu `#5b9ee0`, barre neutre `#5b7d9e`; font IBM Plex Sans / IBM Plex Mono (Google Fonts, fallback `system-ui`/`monospace`).
- Range validati lato firmware: altitude 0–3000, pressure 700–1200, vocIndexOffset 1–250, learningTime 1–1000, gating 0–3000, stdInitial 10–5000, gainFactor 1–1000, noxIndexOffset 1–250, frcTarget 0–2000.
- NOx: `learningTimeGainHours` sempre 12 e `stdInitial` sempre 50 (fissati nel firmware, mai esposti).
- Chiavi `Preferences` max 15 caratteri (limite NVS).
- Compilazione di verifica dopo ogni task: `arduino-cli compile --fqbn esp32:esp32:esp32 /home/justsalvin/projects/airsensor` → exit 0.
- Commit dopo ogni task con `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

## Contratti API (usati da tutti i task)

`GET /api/data` → `200 application/json`
```json
{"state":"online","ageMs":123456,"co2":620,"temp":22.4,"hum":46.0,
 "pm1":3.1,"pm25":4.2,"pm4":5.0,"pm10":6.8,"voc":87,"nox":12,
 "faultFlags":0}
```
`state` ∈ `online|warmup|calibrating|fault` (BOOT è riportato come `warmup`; `offline` lo decide il client). Campi non validi = `null`. `faultFlags` = registro stato SEN66 (uint32).

`GET /api/history` → `{"interval":1800,"points":48,"series":{"co2":[...],"pm1":[...],"pm25":[...],"pm4":[...],"pm10":[...],"voc":[...],"nox":[...],"temp":[...],"hum":[...]}}` — 48 valori dal più vecchio al più recente, `null` per bucket vuoti.

`GET /api/settings` → tutti i campi della struct `Settings` (vedi Task 3).
`POST /api/settings` (form-urlencoded, solo i campi presenti vengono aggiornati) → `{"ok":true}` o `{"ok":false,"error":"..."}` (HTTP 400 su range non valido, 500 su errore I2C).

`POST /api/cmd` con `cmd=fan|heater|frc|voc_reset|reset_learning|co2_factory_reset|clear_status` → `{"ok":true}`; per `frc`: `{"ok":true,"correction":-12}` oppure `{"ok":false,"error":"FRC failed"}`; per `frc` con misura attiva da <3 min: HTTP 409 `{"ok":false,"error":"run >= 3 min first"}`.

`GET /api/device` → `{"product":"SEN66","serial":"...","fw":"2.3","flags":{"fan":false,"rht":false,"gas":false,"co2a":false,"co2b":false,"pm":false,"speed":false},"rssi":-55,"ip":"192.168.1.50","heap":180000,"uptimeS":3600,"lastFan":1721480000,"lastHeat":0}`

---

### Task 1: Toolchain + rimozione Blynk + web server skeleton con `/api/data`

**Files:**
- Modify: `airsensor.ino`
- Create: `.gitignore` (contenuto: `build/`)

**Interfaces:**
- Produces: struct `Latest latest` (campi `float temp,hum,pm1,pm25,pm4,pm10,voc,nox; int32_t co2; uint32_t updatedAt;` — NAN/-1 = non valido); `WebServer server(80);` `const char* stateApi()`; helper JSON `jsonNum(String&,const char*,float,int)`; scheduling `millis()` al posto di BlynkTimer.

- [ ] **Step 1: Installa toolchain (se assente)**

```bash
command -v arduino-cli || (curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=~/.local/bin sh)
export PATH=$PATH:~/.local/bin
arduino-cli config init --overwrite --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index && arduino-cli core install esp32:esp32
arduino-cli lib install "Sensirion I2C SEN66"
```
Expected: core `esp32:esp32` installato; lib SEN66 + Sensirion Core presenti.

- [ ] **Step 2: Rimuovi Blynk e aggiungi il server**

In `airsensor.ino`:
- Elimina: i 4 `#define BLYNK_*`, `#include <BlynkSimpleEsp32.h>`, `BlynkTimer timer`, le costanti `VP_*`, `COL_*`, le soglie `*_ALERT_*`, i latch `*AlertActive`, `checkAlert()`, `setColorIfChanged()`, `co2Color/pm25Color/vocColor`, le variabili `lastCo2Col/lastPm25Col/lastVocCol`, i tre handler `BLYNK_WRITE(V10/V11/V12)` (la loro logica torna nel Task 5 come comandi HTTP).
- `publishState()` / `publishStatusText()`: rimuovi le chiamate Blynk, tieni i log seriali e l'aggiornamento di `state`/`statePrev`.
- Aggiungi in testa:

```cpp
#include <WebServer.h>
#include <ESPmDNS.h>

WebServer server(80);

struct Latest {
    float temp = NAN, hum = NAN;
    float pm1 = NAN, pm25 = NAN, pm4 = NAN, pm10 = NAN;
    float voc = NAN, nox = NAN;
    int32_t co2 = -1;                 // -1 = not valid yet
    uint32_t updatedAt = 0;           // millis() of last good read
};
static Latest latest;
static uint32_t deviceStatusRaw = 0;  // refreshed each sendData()
```

- In `sendData()`: al posto di ogni `Blynk.virtualWrite`, scrivi il campo di `latest` (stesse regole di validità esistenti: temp/hum sempre, PM solo dopo `WARMUP_PM_MS`, co2 se `!=0xFFFF && <=40000`, voc/nox se `plausible(v,1,500)`); i campi non validi restano NAN/-1 in quel giro? NO: assegna il valore solo se valido, altrimenti lascia l'ultimo buono e aggiorna `latest.updatedAt` solo se `readMeasuredValues` è andata a buon fine. In coda aggiorna `deviceStatusRaw` con `readDeviceStatus`:

```cpp
    SEN66DeviceStatus ds;
    if (sensor.readDeviceStatus(ds) == NO_ERROR) deviceStatusRaw = ds.value;
```

- Helper JSON e stato:

```cpp
static const char* stateApi() {
    switch (state) {
        case SensorState::READY:           return "online";
        case SensorState::GAS_CALIBRATION: return "calibrating";
        case SensorState::FAULT:           return "fault";
        default:                           return "warmup";   // BOOT + WARMUP
    }
}

// Appends "key":value or "key":null. dec = decimals.
static void jsonNum(String& s, const char* key, float v, int dec) {
    s += '"'; s += key; s += "\":";
    if (isnan(v)) s += "null"; else s += String(v, dec);
    s += ',';
}
```

- Handler `/api/data`:

```cpp
static void handleApiData() {
    String s; s.reserve(320);
    s += "{\"state\":\""; s += stateApi(); s += "\",";
    s += "\"ageMs\":"; s += String(millis() - measStartMs); s += ',';
    if (latest.co2 >= 0) { s += "\"co2\":"; s += String(latest.co2); s += ','; }
    else                 { s += "\"co2\":null,"; }
    jsonNum(s, "temp", latest.temp, 1);
    jsonNum(s, "hum",  latest.hum, 1);
    jsonNum(s, "pm1",  latest.pm1, 1);
    jsonNum(s, "pm25", latest.pm25, 1);
    jsonNum(s, "pm4",  latest.pm4, 1);
    jsonNum(s, "pm10", latest.pm10, 1);
    jsonNum(s, "voc",  latest.voc, 0);
    jsonNum(s, "nox",  latest.nox, 0);
    s += "\"faultFlags\":"; s += String(deviceStatusRaw); s += '}';
    server.send(200, "application/json", s);
}
```

- `setup()`: sostituisci `Blynk.begin(...)` con:

```cpp
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print('.'); }
    Serial.printf("\nConnected: %s\n", WiFi.localIP().toString().c_str());
    if (MDNS.begin("airsensor")) MDNS.addService("http", "tcp", 80);

    server.on("/api/data", HTTP_GET, handleApiData);
    server.begin();
```

- Scheduling senza BlynkTimer (rimpiazza `timer.setInterval` e `loop()`):

```cpp
void loop() {
    server.handleClient();
    static uint32_t tSend = 0, tMaint = 0;
    uint32_t now = millis();
    if (now - tSend  >= SEND_INTERVAL_MS) { tSend  = now; sendData(); }
    if (now - tMaint >= MAINT_CHECK_MS)   { tMaint = now; runMaintenanceIfDue(); }
}
```

- [ ] **Step 3: Compila**

Run: `arduino-cli compile --fqbn esp32:esp32:esp32 /home/justsalvin/projects/airsensor`
Expected: exit 0 (warning ammessi).

- [ ] **Step 4: Commit**

```bash
git add -A && git commit -m "feat: replace Blynk with local web server exposing /api/data"
```

---

### Task 2: Storico 24h + `/api/history`

**Files:**
- Modify: `airsensor.ino`

**Interfaces:**
- Consumes: `latest`, `sendData()` del Task 1.
- Produces: `enum Metric { M_CO2, M_PM1, M_PM25, M_PM4, M_PM10, M_VOC, M_NOX, M_TEMP, M_HUM, METRIC_COUNT }`; `void histAdd(Metric, float)`; `void histTick()`; endpoint `/api/history`.

- [ ] **Step 1: Ring buffer**

```cpp
enum Metric { M_CO2, M_PM1, M_PM25, M_PM4, M_PM10, M_VOC, M_NOX, M_TEMP, M_HUM, METRIC_COUNT };
static constexpr int      HIST_POINTS = 48;
static constexpr uint32_t BUCKET_MS   = 30UL * 60 * 1000;

static float    histSum[METRIC_COUNT][HIST_POINTS] = {};
static uint16_t histCnt[METRIC_COUNT][HIST_POINTS] = {};
static int      histHead = 0;
static uint32_t bucketStartMs = 0;

static void histAdd(Metric m, float v) {
    histSum[m][histHead] += v;
    histCnt[m][histHead]++;
}

static void histTick() {
    if (millis() - bucketStartMs < BUCKET_MS) return;
    bucketStartMs += BUCKET_MS;
    histHead = (histHead + 1) % HIST_POINTS;
    for (int m = 0; m < METRIC_COUNT; m++) { histSum[m][histHead] = 0; histCnt[m][histHead] = 0; }
}
```

In `setup()` inizializza `bucketStartMs = millis();`. In `loop()` chiama `histTick();` a ogni giro. In `sendData()`, dopo l'aggiornamento di `latest`, per ogni valore valido chiama `histAdd` (es. `histAdd(M_CO2, co2)` dentro il ramo che valida co2, `histAdd(M_PM25, pm2p5)` nel ramo PM, ecc.).

- [ ] **Step 2: Endpoint**

```cpp
static const char* const METRIC_KEYS[METRIC_COUNT] =
    { "co2", "pm1", "pm25", "pm4", "pm10", "voc", "nox", "temp", "hum" };

static void handleApiHistory() {
    String s; s.reserve(4500);
    s += "{\"interval\":1800,\"points\":48,\"series\":{";
    for (int m = 0; m < METRIC_COUNT; m++) {
        s += '"'; s += METRIC_KEYS[m]; s += "\":[";
        for (int i = 1; i <= HIST_POINTS; i++) {          // oldest -> newest
            int idx = (histHead + i) % HIST_POINTS;
            if (histCnt[m][idx] == 0) s += "null";
            else s += String(histSum[m][idx] / histCnt[m][idx], m == M_CO2 ? 0 : 1);
            if (i < HIST_POINTS) s += ',';
        }
        s += ']'; if (m < METRIC_COUNT - 1) s += ',';
    }
    s += "}}";
    server.send(200, "application/json", s);
}
```

Registra `server.on("/api/history", HTTP_GET, handleApiHistory);` in `setup()`.

- [ ] **Step 3: Compila** — stesso comando, exit 0.
- [ ] **Step 4: Commit** — `feat: add 24h ring-buffer history and /api/history`

---

### Task 3: Settings persistiti + applicazione a boot + `/api/settings`

**Files:**
- Modify: `airsensor.ino`

**Interfaces:**
- Consumes: `check()`, `sensor`, `prefs`, `pauseMeasurement()`, `resumeMeasurement()` esistenti.
- Produces: `struct Settings cfg`; `void loadSettings()`, `void saveSettings()`, `void applyIdleSettings()`, `void applyAnyStateSettings()`; endpoint GET/POST `/api/settings`. Il campo `cfg.room` sostituisce il "Living room" fisso in `/api/data`.

- [ ] **Step 1: Struct + load/save**

```cpp
struct Settings {
    uint16_t altitude    = 120;      // m, 0-3000 (default = SENSOR_ALTITUDE_M)
    bool     usePressure = false;    // true: ambient pressure overrides altitude
    uint16_t pressure    = 1013;     // hPa, 700-1200
    float    tempOffset  = 0.0f;     // degC, firmware scales x200
    float    tempSlope   = 0.0f;     // normalized, firmware scales x10000
    bool     vocPersist  = true;     // auto save/restore VOC learning state
    bool     asc         = true;     // CO2 automatic self calibration
    uint16_t frcTarget   = 420;      // ppm used by the FRC command
    int16_t  vocIdxOff = 100, vocLearnOff = 12, vocLearnGain = 12,
             vocGateMax = 180, vocStdInit = 50, vocGain = 230;
    int16_t  noxIdxOff = 1, noxLearnOff = 12, noxGateMax = 720, noxGain = 230;
};
static Settings cfg;

static void loadSettings() {
    cfg.altitude    = prefs.getUShort("alt",      cfg.altitude);
    cfg.usePressure = prefs.getBool  ("usePress", cfg.usePressure);
    cfg.pressure    = prefs.getUShort("press",    cfg.pressure);
    cfg.tempOffset  = prefs.getFloat ("tOff",     cfg.tempOffset);
    cfg.tempSlope   = prefs.getFloat ("tSlope",   cfg.tempSlope);
    cfg.vocPersist  = prefs.getBool  ("vocPersist", cfg.vocPersist);
    cfg.asc         = prefs.getBool  ("asc",      cfg.asc);
    cfg.frcTarget   = prefs.getUShort("frcTarget", cfg.frcTarget);
    cfg.vocIdxOff   = prefs.getShort ("vocIdxOff",  cfg.vocIdxOff);
    cfg.vocLearnOff = prefs.getShort ("vocLearnOff", cfg.vocLearnOff);
    cfg.vocLearnGain= prefs.getShort ("vocLearnGain", cfg.vocLearnGain);
    cfg.vocGateMax  = prefs.getShort ("vocGateMax", cfg.vocGateMax);
    cfg.vocStdInit  = prefs.getShort ("vocStdInit", cfg.vocStdInit);
    cfg.vocGain     = prefs.getShort ("vocGain",   cfg.vocGain);
    cfg.noxIdxOff   = prefs.getShort ("noxIdxOff", cfg.noxIdxOff);
    cfg.noxLearnOff = prefs.getShort ("noxLearnOff", cfg.noxLearnOff);
    cfg.noxGateMax  = prefs.getShort ("noxGateMax", cfg.noxGateMax);
    cfg.noxGain     = prefs.getShort ("noxGain",   cfg.noxGain);
}
static void saveSettings() {
    prefs.putUShort("alt", cfg.altitude);      prefs.putBool("usePress", cfg.usePressure);
    prefs.putUShort("press", cfg.pressure);
    prefs.putFloat("tOff", cfg.tempOffset);    prefs.putFloat("tSlope", cfg.tempSlope);
    prefs.putBool("vocPersist", cfg.vocPersist); prefs.putBool("asc", cfg.asc);
    prefs.putUShort("frcTarget", cfg.frcTarget);
    prefs.putShort("vocIdxOff", cfg.vocIdxOff);  prefs.putShort("vocLearnOff", cfg.vocLearnOff);
    prefs.putShort("vocLearnGain", cfg.vocLearnGain); prefs.putShort("vocGateMax", cfg.vocGateMax);
    prefs.putShort("vocStdInit", cfg.vocStdInit); prefs.putShort("vocGain", cfg.vocGain);
    prefs.putShort("noxIdxOff", cfg.noxIdxOff);  prefs.putShort("noxLearnOff", cfg.noxLearnOff);
    prefs.putShort("noxGateMax", cfg.noxGateMax); prefs.putShort("noxGain", cfg.noxGain);
}
```

- [ ] **Step 2: Applicazione**

```cpp
// Idle-only commands. Call between stopMeasurement() and startContinuousMeasurement().
static void applyIdleSettings() {
    check(sensor.setSensorAltitude(cfg.altitude), "setSensorAltitude()");
    check(sensor.setVocAlgorithmTuningParameters(cfg.vocIdxOff, cfg.vocLearnOff,
          cfg.vocLearnGain, cfg.vocGateMax, cfg.vocStdInit, cfg.vocGain),
          "setVocAlgorithmTuningParameters()");
    // NOx: learningTimeGainHours must be 12, stdInitial must be 50 (datasheet)
    check(sensor.setNoxAlgorithmTuningParameters(cfg.noxIdxOff, cfg.noxLearnOff,
          12, cfg.noxGateMax, 50, cfg.noxGain),
          "setNoxAlgorithmTuningParameters()");
    check(sensor.setCo2SensorAutomaticSelfCalibration(cfg.asc ? 1 : 0),
          "setCo2SensorAutomaticSelfCalibration()");
}

// Commands accepted in any state.
static void applyAnyStateSettings() {
    if (cfg.usePressure)
        check(sensor.setAmbientPressure(cfg.pressure), "setAmbientPressure()");
    check(sensor.setTemperatureOffsetParameters(
          (int16_t)lroundf(cfg.tempOffset * 200.0f),
          (int16_t)lroundf(cfg.tempSlope * 10000.0f), 0, 0),
          "setTemperatureOffsetParameters()");
}
```

In `startSensor()`, sostituisci la riga `setSensorAltitude(SENSOR_ALTITUDE_M)` con `applyIdleSettings(); applyAnyStateSettings();` (prima di `startContinuousMeasurement`). In `setup()`, chiama `loadSettings()` subito dopo `prefs.begin(...)`. Rimuovi la costante `SENSOR_ALTITUDE_M` (il default vive in `Settings`). Nessun concetto di "room" nel progetto: se `handleApiData()` contiene ancora un campo `room`, rimuovilo.

- [ ] **Step 3: Endpoint GET/POST**

GET serializza tutti i campi con le stesse chiavi accettate dal POST: `altitude, usePressure, pressure, tempOffset, tempSlope, vocPersist, asc, frcTarget, vocIdxOff, vocLearnOff, vocLearnGain, vocGateMax, vocStdInit, vocGain, noxIdxOff, noxLearnOff, noxGateMax, noxGain` (bool come `0/1`). Parsing POST completo:

```cpp
// Parse helpers: absent arg = keep current value; out-of-range = reject.
static bool argU16(const char* n, uint16_t& out, long lo, long hi, String& bad) {
    if (!server.hasArg(n)) return true;
    long v = server.arg(n).toInt();
    if (v < lo || v > hi) { bad = n; return false; }
    out = (uint16_t)v; return true;
}
static bool argI16(const char* n, int16_t& out, long lo, long hi, String& bad) {
    if (!server.hasArg(n)) return true;
    long v = server.arg(n).toInt();
    if (v < lo || v > hi) { bad = n; return false; }
    out = (int16_t)v; return true;
}
static bool argF(const char* n, float& out, float lo, float hi, String& bad) {
    if (!server.hasArg(n)) return true;
    float v = server.arg(n).toFloat();
    if (isnan(v) || v < lo || v > hi) { bad = n; return false; }
    out = v; return true;
}
static bool argB(const char* n, bool& out) {
    if (server.hasArg(n)) out = server.arg(n).toInt() != 0;
    return true;
}

static void handleApiSettingsPost() {
    Settings c = cfg;                      // work on a copy, commit only if all valid
    String bad;
    bool ok = argU16("altitude", c.altitude, 0, 3000, bad)
           && argU16("pressure", c.pressure, 700, 1200, bad)
           && argU16("frcTarget", c.frcTarget, 0, 2000, bad)
           && argF("tempOffset", c.tempOffset, -10, 10, bad)
           && argF("tempSlope",  c.tempSlope, -1, 1, bad)
           && argI16("vocIdxOff",   c.vocIdxOff, 1, 250, bad)
           && argI16("vocLearnOff", c.vocLearnOff, 1, 1000, bad)
           && argI16("vocLearnGain",c.vocLearnGain, 1, 1000, bad)
           && argI16("vocGateMax",  c.vocGateMax, 0, 3000, bad)
           && argI16("vocStdInit",  c.vocStdInit, 10, 5000, bad)
           && argI16("vocGain",     c.vocGain, 1, 1000, bad)
           && argI16("noxIdxOff",   c.noxIdxOff, 1, 250, bad)
           && argI16("noxLearnOff", c.noxLearnOff, 1, 1000, bad)
           && argI16("noxGateMax",  c.noxGateMax, 0, 3000, bad)
           && argI16("noxGain",     c.noxGain, 1, 1000, bad);
    argB("usePressure", c.usePressure);
    argB("vocPersist",  c.vocPersist);
    argB("asc",         c.asc);
    if (!ok) { sendErr(400, ("invalid " + bad).c_str()); return; }

    // Idle-only commands needed only if one of these actually changed.
    bool needIdle = c.altitude != cfg.altitude || c.asc != cfg.asc
        || c.vocIdxOff != cfg.vocIdxOff || c.vocLearnOff != cfg.vocLearnOff
        || c.vocLearnGain != cfg.vocLearnGain || c.vocGateMax != cfg.vocGateMax
        || c.vocStdInit != cfg.vocStdInit || c.vocGain != cfg.vocGain
        || c.noxIdxOff != cfg.noxIdxOff || c.noxLearnOff != cfg.noxLearnOff
        || c.noxGateMax != cfg.noxGateMax || c.noxGain != cfg.noxGain;
    bool persistTurnedOff = cfg.vocPersist && !c.vocPersist;

    cfg = c;
    saveSettings();
    if (persistTurnedOff) prefs.remove("vocState");   // key defined in Task 4
    if (needIdle) {
        pauseMeasurement();
        applyIdleSettings();
        applyAnyStateSettings();
        resumeMeasurement();          // PM warmup restarts, state machine follows
    } else {
        applyAnyStateSettings();
    }
    server.send(200, "application/json", "{\"ok\":true}");
}
```

Nota: `sendErr` è definita nel Task 5 — se questo task viene eseguito prima, includerla qui con la stessa firma (`static void sendErr(int code, const char* msg)`).

- [ ] **Step 4: Compila** — exit 0.
- [ ] **Step 5: Commit** — `feat: persist SEN66 settings and expose /api/settings`

---

### Task 4: Persistenza automatica stato VOC

**Files:**
- Modify: `airsensor.ino`

**Interfaces:**
- Consumes: `cfg.vocPersist` (Task 3), `startSensor()`, scheduling in `loop()`.
- Produces: chiave Preferences `vocState` (blob 8 byte); `void saveVocState()`; restore automatico in `startSensor()`.

- [ ] **Step 1: Save/restore**

```cpp
static constexpr uint32_t VOC_SAVE_MS = 60UL * 60 * 1000;   // hourly

static void saveVocState() {
    if (!cfg.vocPersist) return;
    if (state != SensorState::READY && state != SensorState::GAS_CALIBRATION) return;
    uint8_t st[8];
    if (check(sensor.getVocAlgorithmState(st, sizeof st), "getVocAlgorithmState()"))
        prefs.putBytes("vocState", st, sizeof st);
}
```

In `startSensor()`, subito prima di `startContinuousMeasurement()`:

```cpp
    if (cfg.vocPersist && prefs.getBytesLength("vocState") == 8) {
        uint8_t st[8];
        prefs.getBytes("vocState", st, sizeof st);
        if (check(sensor.setVocAlgorithmState(st, sizeof st), "setVocAlgorithmState()"))
            Serial.println("VOC learning state restored.");
    }
```

In `loop()` aggiungi il timer: `static uint32_t tVoc = 0; if (now - tVoc >= VOC_SAVE_MS) { tVoc = now; saveVocState(); }`

- [ ] **Step 2: Compila** — exit 0.
- [ ] **Step 3: Commit** — `feat: automatic VOC learning-state persistence`

---

### Task 5: Comandi `/api/cmd` + `/api/device`

**Files:**
- Modify: `airsensor.ino`

**Interfaces:**
- Consumes: `doFanCleaning()`, `doShtHeater()`, `pauseMeasurement()`, `resumeMeasurement()`, `applyIdleSettings()`, `applyAnyStateSettings()`, `cfg`, `MIN_RUN_BEFORE_FRC`, `deviceStatusRaw`.
- Produces: endpoint `/api/cmd` (POST, campo `cmd`) e `/api/device` (GET) con i contratti definiti sopra.

- [ ] **Step 1: `/api/cmd`**

```cpp
static void sendOk()                  { server.send(200, "application/json", "{\"ok\":true}"); }
static void sendErr(int code, const char* msg) {
    String s = "{\"ok\":false,\"error\":\""; s += msg; s += "\"}";
    server.send(code, "application/json", s);
}

// Full restart used by the reset commands: reset chip, reapply, remeasure.
static void restartAfterDeviceReset() {
    check(sensor.deviceReset(), "deviceReset()");
    delay(1500);
    publishState(SensorState::BOOT);
    if (startSensor()) publishState(SensorState::WARMUP);
}

static void handleApiCmd() {
    String cmd = server.arg("cmd");
    if (cmd == "fan") {
        publishStatusText("Cleaning fan...");
        pauseMeasurement(); doFanCleaning(); resumeMeasurement();
        sendOk();
    } else if (cmd == "heater") {
        publishStatusText("SHT heater cycle...");
        pauseMeasurement(); doShtHeater(); resumeMeasurement();
        sendOk();
    } else if (cmd == "frc") {
        if (millis() - measStartMs < MIN_RUN_BEFORE_FRC) { sendErr(409, "run >= 3 min first"); return; }
        publishStatusText("CO2 recalibration...");
        pauseMeasurement();
        uint16_t corr = 0;
        bool ok = check(sensor.performForcedCo2Recalibration(cfg.frcTarget, corr),
                        "performForcedCo2Recalibration()") && corr != 0xFFFF;
        delay(500);
        resumeMeasurement();
        if (!ok) { sendErr(500, "FRC failed"); return; }
        String s = "{\"ok\":true,\"correction\":"; s += String((int)corr - 0x8000); s += '}';
        server.send(200, "application/json", s);
    } else if (cmd == "voc_reset") {
        // Lose only VOC/NOx learning; keep every persisted setting.
        publishStatusText("Resetting VOC learning...");
        prefs.remove("vocState");
        pauseMeasurement();
        restartAfterDeviceReset();
        sendOk();
    } else if (cmd == "reset_learning") {
        // Tuning back to defaults + learning wiped; maintenance dates kept.
        publishStatusText("Resetting learning...");
        Settings fresh;
        cfg = fresh;
        saveSettings();
        prefs.remove("vocState");
        pauseMeasurement();
        restartAfterDeviceReset();
        sendOk();
    } else if (cmd == "co2_factory_reset") {
        publishStatusText("CO2 factory reset...");
        pauseMeasurement();
        bool ok = check(sensor.performCo2SensorFactoryReset(), "performCo2SensorFactoryReset()");
        resumeMeasurement();
        ok ? sendOk() : sendErr(500, "co2 factory reset failed");
    } else if (cmd == "clear_status") {
        SEN66DeviceStatus ds;
        bool ok = check(sensor.readAndClearDeviceStatus(ds), "readAndClearDeviceStatus()");
        if (ok) deviceStatusRaw = 0;
        ok ? sendOk() : sendErr(500, "clear failed");
    } else {
        sendErr(400, "unknown cmd");
    }
}
```

- [ ] **Step 2: `/api/device`**

```cpp
static void handleApiDevice() {
    int8_t name[32] = {0}, serial[32] = {0};
    uint8_t fwMaj = 0, fwMin = 0;
    sensor.getProductName(name, sizeof name);
    sensor.getSerialNumber(serial, sizeof serial);
    sensor.getVersion(fwMaj, fwMin);
    SEN66DeviceStatus ds; ds.value = deviceStatusRaw;
    String s; s.reserve(420);
    s += "{\"product\":\""; s += (const char*)name;
    s += "\",\"serial\":\""; s += (const char*)serial;
    s += "\",\"fw\":\""; s += String(fwMaj); s += '.'; s += String(fwMin); s += "\",";
    s += "\"flags\":{";
    s += "\"fan\":";   s += ds.fanError ? "true," : "false,";
    s += "\"rht\":";   s += ds.rhtError ? "true," : "false,";
    s += "\"gas\":";   s += ds.gasError ? "true," : "false,";
    s += "\"co2a\":";  s += ds.co21Error ? "true," : "false,";
    s += "\"co2b\":";  s += ds.co22Error ? "true," : "false,";
    s += "\"pm\":";    s += ds.pmError ? "true," : "false,";
    s += "\"speed\":"; s += ds.fanSpeedWarning ? "true" : "false";
    s += "},";
    s += "\"rssi\":"; s += String(WiFi.RSSI()); s += ',';
    s += "\"ip\":\""; s += WiFi.localIP().toString(); s += "\",";
    s += "\"heap\":"; s += String(ESP.getFreeHeap()); s += ',';
    s += "\"uptimeS\":"; s += String(millis() / 1000); s += ',';
    s += "\"lastFan\":"; s += String((long long)prefs.getLong64("lastFan", 0)); s += ',';
    s += "\"lastHeat\":"; s += String((long long)prefs.getLong64("lastHeat", 0)); s += '}';
    server.send(200, "application/json", s);
}
```

Nota: `getProductName`/`getSerialNumber`/`getVersion` sono lette a ogni richiesta; funzionano anche in misura. Registra entrambi gli endpoint in `setup()`.

- [ ] **Step 3: Compila** — exit 0.
- [ ] **Step 4: Commit** — `feat: admin commands /api/cmd and device info /api/device`

---

### Task 6: Dashboard — `dev/dashboard.html`, mock server, embed in PROGMEM

**Files:**
- Create: `dev/dashboard.html`, `dev/mock_server.py`, `dev/embed.py`
- Create: `dashboard_html.h` (generato da `embed.py`)
- Modify: `airsensor.ino` (route `/`)

**Interfaces:**
- Consumes: contratti `/api/data` e `/api/history`.
- Produces: `dashboard_html.h` con `static const char DASHBOARD_HTML[] PROGMEM = R"HTMLDOC(...)HTMLDOC";`; route `server.on("/", HTTP_GET, ...)` che risponde `server.send_P(200, "text/html", DASHBOARD_HTML);`.

- [ ] **Step 1: `dev/dashboard.html`**

Porta `docs/design/air-dashboard-v2.dc.html` (mobile) a HTML/JS vanilla; per il layout desktop il mockup autoritativo è `docs/design/air-dashboard-desktop.dc.html`. Il titolo dell'header è fisso: "Air Sensor" (mai nomi di stanza). Regole di porting:
- Gli stili inline del design diventano classi CSS in un `<style>` (stessi valori esatti); `style-hover` → `:hover`; la direttiva `sc-if` → `hidden`/render condizionale JS; `sc-for` → template JS.
- Le funzioni `co2Band`, `pm25Band`, `idxBand`, la mappa `states`, `chart(pts,color)` e `gaugeEl(v,color)` si portano 1:1 (da `React.createElement` a costruzione SVG via template string), inclusi i binding NOx `idxBand(nox*3)` e le percentuali barre (`pm25/55`, `voc/500`, `nox/150`, `(temp-10)/25`, `hum`).
- `series()` (dati finti) NON si porta: il grafico usa `/api/history`.
- Logica dati:

```js
const POLL_MS = 5000;
let fails = 0, lastData = null, lastOkAt = 0;
async function poll() {
  try {
    const r = await fetch('/api/data'); if (!r.ok) throw 0;
    lastData = await r.json(); lastOkAt = Date.now(); fails = 0;
  } catch (e) { fails++; }
  render();
}
setInterval(poll, POLL_MS); poll();
// stato effettivo mostrato: 'offline' se fails >= 3, altrimenti lastData.state
// lastUpdate: 'just now' se <10 s da lastOkAt, poi 'Xs ago' / 'Xm ago' / 'Xh ago'
```

- Valori `null` → placeholder `—` (e barra a 0, gauge a fondo scala minimo).
- Dettaglio: al tap su una card fetch di `/api/history`, serie della metrica filtrata dei `null` per min/avg/max; i `null` nel grafico spezzano la linea (segmenti separati); asse temporale fisso -24h→now come nel design.
- Banner fault: visibile quando `state=='fault'` o `faultFlags!=0`; mappa flag→messaggio:

```js
function faultInfo(flags) {
  if (flags & (1 << 4))  return { code: 'E-FAN', msg: 'Fan failure — check the air inlet' };
  if (flags & (1 << 11)) return { code: 'E-PM',  msg: 'PM sensor error' };
  if (flags & (1 << 6))  return { code: 'E-RHT', msg: 'Temperature/humidity sensor error' };
  if (flags & (1 << 7))  return { code: 'E-GAS', msg: 'VOC/NOx sensor error' };
  if (flags & ((1 << 9) | (1 << 12))) return { code: 'E-CO2', msg: 'CO2 sensor error' };
  if (flags & (1 << 21)) return { code: 'W-SPD', msg: 'Fan speed out of range' };
  return { code: 'E-217', msg: 'Sensor fault' };
}
```

- Desktop `@media (min-width:700px)`: replica `docs/design/air-dashboard-desktop.dc.html` (stessa pagina, layout che si riorganizza via CSS; il bottom-sheet diventa dialog centrato).
- Footer: link discreto `Maintenance` → `/admin` (font 11px, colore `#5a6675`).
- `<link>` Google Fonts con `onerror` silenzioso; `font-family` sempre con fallback.

- [ ] **Step 2: `dev/mock_server.py`**

```python
#!/usr/bin/env python3
"""Dev server: serves dev/*.html plus fake /api endpoints on :8000."""
import json, math, random, time
from http.server import HTTPServer, SimpleHTTPRequestHandler

STATE = 'online'   # cambia a mano per testare warmup/calibrating/fault

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
```

Verifica: `python3 dev/mock_server.py &` poi `curl -s localhost:8000/api/data | python3 -m json.tool` (JSON valido) e `curl -s localhost:8000/ | head -5` (HTML della dashboard).

- [ ] **Step 3: `dev/embed.py`**

```python
#!/usr/bin/env python3
"""Wraps dev/*.html into PROGMEM headers for the sketch."""
import pathlib
ROOT = pathlib.Path(__file__).resolve().parent.parent
for src, dst, sym in [("dev/dashboard.html", "dashboard_html.h", "DASHBOARD_HTML"),
                      ("dev/admin.html", "admin_html.h", "ADMIN_HTML")]:
    p = ROOT / src
    if not p.exists():
        continue
    html = p.read_text()
    assert ")HTMLDOC" not in html, f"{src}: raw-literal delimiter collision"
    (ROOT / dst).write_text(
        "// AUTO-GENERATED from " + src + " by dev/embed.py - DO NOT EDIT\n"
        "#pragma once\n"
        f'static const char {sym}[] PROGMEM = R"HTMLDOC({html})HTMLDOC";\n')
    print(f"{dst}: {len(html)} bytes")
```

Run: `python3 dev/embed.py` → stampa la dimensione, crea `dashboard_html.h`.

- [ ] **Step 4: Route nel firmware**

In `airsensor.ino`: `#include "dashboard_html.h"` e in `setup()`:

```cpp
    server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", DASHBOARD_HTML); });
```

- [ ] **Step 5: Verifica browser (mock) + compila**

Con il mock server attivo, controlla a occhio su `http://localhost:8000/`: gauge, card, dettaglio con grafico, stati (cambiando `STATE` nel mock), viewport 430px e >700px. Poi `arduino-cli compile ...` → exit 0.

- [ ] **Step 6: Commit** — `feat: dashboard web UI (design Air Dashboard v2) served from PROGMEM`

---

### Task 7: Pannello `/admin` — `dev/admin.html`

**Files:**
- Create: `dev/admin.html`
- Create: `admin_html.h` (generato)
- Modify: `airsensor.ino` (route `/admin`)

**Interfaces:**
- Consumes: `/api/settings` GET/POST, `/api/cmd`, `/api/device`; `dev/embed.py`.
- Produces: route `server.on("/admin", ...)` → `send_P(200, "text/html", ADMIN_HTML)`.

- [ ] **Step 1: `dev/admin.html`**

Mockup autoritativi: `docs/design/air-admin.dc.html` (mobile) e `docs/design/air-admin-desktop.dc.html` (desktop) — replicali per layout, copy e componenti (senza alcun riferimento a nomi di stanza). Stesso `<style>` di base della dashboard (palette, font, card). Contenuto funzionale richiesto (deve coincidere coi mockup; in caso di divergenza vincono i mockup):

1. **CO₂ calibration** — numero `frcTarget` (0–2000, default 420) + bottone `Recalibrate now` (conferma via `confirm()`; mostra la correzione ppm restituita o l'errore 409 "run >= 3 min first"); toggle ASC; bottone `CO₂ factory reset` (rosso, doppio `confirm()`).
2. **Environment compensation** — numero altitude (0–3000 m); toggle `Use ambient pressure`; numero pressure (700–1200 hPa, abilitato solo con toggle on); numeri tempOffset (−10…10 °C, step 0.1) e tempSlope (−1…1, step 0.001).
3. **VOC tuning** — 6 numeri con range/default nei label (`indexOffset 1–250 · default 100`, ecc.); in fondo: toggle `Persist VOC learning` + bottone `Reset VOC learning` (conferma).
4. **NOx tuning** — 4 numeri (indexOffset def 1, learningTimeOffsetHours def 12, gatingMaxDurationMinutes def 720, gainFactor def 230).
5. **Maintenance** — bottoni `Clean fan now` (~15 s), `SHT heater cycle` (~25 s), `Reset all learning` (rosso, conferma); date `lastFan`/`lastHeat` formattate da epoch (0 → `never`); nota che fan/heater girano comunque in automatico (settimanale/mensile).
6. **Device** (sola lettura) — product/serial/fw, griglia flag con dot verde/rosso (fan, rht, gas, co2a, co2b, pm, speed), rssi, ip, heap, uptime; bottone `Clear sticky flags`.

Comportamento JS: al load `GET /api/settings` + `GET /api/device` e popola; ogni sezione con campi ha un bottone `Save` che POSTa SOLO i campi della sezione (`URLSearchParams`); i bottoni azione POSTano `/api/cmd`. Durante un'azione: bottone disabilitato con testo `Working…`; esito come toast in basso (verde ok / rosso errore con messaggio del firmware). I toggle sono switch CSS (stile pill del design). Bottoni: default bordo `#2c3b4e` su `#18202b`; distruttivi testo `#e06e5b` bordo `rgba(224,110,91,.4)`.

- [ ] **Step 2: Embed + route**

`python3 dev/embed.py` → genera anche `admin_html.h`. Nel firmware: `#include "admin_html.h"` + `server.on("/admin", HTTP_GET, []() { server.send_P(200, "text/html", ADMIN_HTML); });`

- [ ] **Step 3: Verifica browser (mock) + compila**

Su `http://localhost:8000/admin`: tutte le sezioni popolate dai dati mock, POST simulati con esito ok. Poi `arduino-cli compile ...` → exit 0.

- [ ] **Step 4: Commit** — `feat: /admin maintenance and calibration panel`

---

### Task 8: PWA + rifiniture finali

**Files:**
- Modify: `dev/dashboard.html`, `airsensor.ino`
- Create: `README.md`

**Interfaces:**
- Consumes: tutto quanto sopra.
- Produces: `/manifest.json`; meta PWA nella dashboard; README.

- [ ] **Step 1: Manifest + meta**

In `airsensor.ino`:

```cpp
static const char MANIFEST_JSON[] PROGMEM = R"JSON({
  "name": "Air Sensor", "short_name": "Air",
  "display": "standalone", "start_url": "/",
  "background_color": "#0d1117", "theme_color": "#0d1117",
  "icons": [{ "src": "/icon.svg", "sizes": "any", "type": "image/svg+xml" }]
})JSON";
static const char ICON_SVG[] PROGMEM =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'>"
    "<rect width='100' height='100' rx='22' fill='#141b24'/>"
    "<circle cx='50' cy='50' r='26' fill='none' stroke='#4fc98f' stroke-width='9'/>"
    "<circle cx='50' cy='50' r='7' fill='#4fc98f'/></svg>";
```

Route: `/manifest.json` → `send_P(200, "application/manifest+json", MANIFEST_JSON)`; `/icon.svg` → `send_P(200, "image/svg+xml", ICON_SVG)`.
In `dev/dashboard.html` `<head>`: `<link rel="manifest" href="/manifest.json">`, `<meta name="apple-mobile-web-app-capable" content="yes">`, `<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">`, `<meta name="theme-color" content="#0d1117">`, `<link rel="apple-touch-icon" href="/icon.svg">`. Rigenera con `dev/embed.py`.

- [ ] **Step 2: 404 + README**

`server.onNotFound(...)` → redirect 302 a `/`. README breve: cosa fa, come si compila (`arduino-cli` o IDE), come si sviluppa la UI (`dev/mock_server.py` + `dev/embed.py`), URL `http://airsensor.local`, nota sicurezza: nessuna auth, solo LAN fidata; le credenziali WiFi sono nel sorgente.

- [ ] **Step 3: Compila + commit finale**

`arduino-cli compile ...` → exit 0, annota flash/RAM usage dal report. Commit: `feat: PWA manifest, icon and project README`

---

### Task 9: UAT su dispositivo reale (manuale, con l'utente)

Nessun codice. Checklist da consegnare all'utente:

- [ ] Flash dello sketch (IDE Arduino o `arduino-cli upload`), monitor seriale: WiFi connesso, IP stampato, "Measurement started."
- [ ] `http://airsensor.local` e `http://<ip>` aprono la dashboard; pill `Warming up` → `Calibrating` → (dopo 6h) `Online`.
- [ ] Valori coerenti col monitor seriale; dettaglio per ogni metrica; dopo ≥30 min il grafico ha punti reali.
- [ ] `/admin`: modifica altitudine → salva → ricompare al reload e dopo un riavvio (settaggi riapplicati).
- [ ] `Clean fan now` udibile (~10 s a velocità massima), poi misura riprende.
- [ ] Riavvio: log "VOC learning state restored."
- [ ] iPhone: Condividi → Aggiungi a Home; l'app si apre standalone a tema scuro.
- [ ] Spegni l'ESP32: la dashboard aperta passa a `Offline` entro ~15 s.
