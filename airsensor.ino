// ============================================================
//  Air Quality Monitor - SEN66 + ESP32 + local web server
// ============================================================
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include <Preferences.h>
#include <SensirionI2cSen66.h>
#include "dashboard_html.h"
#include "admin_html.h"

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

#ifdef NO_ERROR
#undef NO_ERROR
#endif
#define NO_ERROR 0

// ============================================================
//  CONFIGURATION - the only section you should ever touch
// ============================================================
char ssid[] = "NETGEAR30";
char pass[] = "TyjH39hFzd";

static constexpr int PIN_SDA = 22;   // green wire
static constexpr int PIN_SCL = 23;   // yellow wire

// ============================================================
//  Internals
// ============================================================
static constexpr uint32_t I2C_FREQ = 100000;

static constexpr long     SEND_INTERVAL_MS   = 10000;
static constexpr long     MAINT_CHECK_MS     = 60UL * 60 * 1000;
static constexpr uint32_t WARMUP_PM_MS       = 30UL * 1000;
static constexpr uint32_t WARMUP_NOX_MS      = 6UL * 60 * 60 * 1000;
static constexpr uint32_t MIN_RUN_BEFORE_FRC = 3UL * 60 * 1000;
static constexpr time_t   FAN_CLEAN_PERIOD_S = 7UL  * 24 * 3600;   // weekly
static constexpr time_t   SHT_HEAT_PERIOD_S  = 30UL * 24 * 3600;   // monthly
static constexpr uint32_t VOC_SAVE_MS        = 60UL * 60 * 1000;   // hourly
static constexpr time_t   TIME_VALID_AFTER   = 1700000000;

enum class SensorState { BOOT, WARMUP, GAS_CALIBRATION, READY, FAULT };

// 24h history ring buffer (30 min buckets, 48 points). Declared here, ahead
// of every function, so the IDE's auto-generated prototypes (which land
// above the first function definition) see the Metric type already defined.
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

// Persisted sensor tuning settings (Preferences-backed). Declared here, ahead
// of every function, for the same auto-prototype-safety reason as the
// history ring buffer above.
struct Settings {
    uint16_t altitude    = 120;      // m, 0-3000 (previous fixed SENSOR_ALTITUDE_M default)
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

SensirionI2cSen66 sensor;
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

Preferences prefs;                     // maintenance dates + persisted Settings

static char errorMessage[128];
static SensorState state     = SensorState::BOOT;
static SensorState statePrev = SensorState::FAULT;
static uint32_t measStartMs  = 0;

// ------------------------------------------------------------
//  Helpers
// ------------------------------------------------------------
static bool check(int16_t err, const char* what) {
    if (err == NO_ERROR) return true;
    Serial.printf("Error in %s: ", what);
    errorToString(err, errorMessage, sizeof errorMessage);
    Serial.println(errorMessage);
    return false;
}

static bool plausible(float v, float lo, float hi) {
    return !isnan(v) && v >= lo && v <= hi;
}

static time_t nowEpoch() {
    time_t t = time(nullptr);
    return (t > TIME_VALID_AFTER) ? t : 0;   // 0 = clock not synced yet
}

static const char* stateText(SensorState s) {
    switch (s) {
        case SensorState::BOOT:            return "Starting...";
        case SensorState::WARMUP:          return "Warming up";
        case SensorState::GAS_CALIBRATION: return "Gas calibration";
        case SensorState::READY:           return "Ready";
        default:                           return "SENSOR FAULT";
    }
}

static void publishState(SensorState s) {
    state = s;
    if (s != statePrev) {
        Serial.printf(">>> State: %s\n", stateText(s));
        statePrev = s;
    }
}

static void publishStatusText(const char* msg) {   // transient messages
    Serial.printf(">>> %s\n", msg);
    statePrev = SensorState::FAULT;   // force next publishState() to log again
}

static bool scanBus() {
    Serial.println("Scanning I2C bus...");
    bool found = false;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  found 0x%02X\n", addr);
            if (addr == 0x6B) found = true;
        }
    }
    if (!found) Serial.println("  >>> SEN66 (0x6B) NOT found.");
    return found;
}

// ------------------------------------------------------------
//  Settings persistence and application
// ------------------------------------------------------------
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

// Idle-only commands. Call between stopMeasurement() and startContinuousMeasurement().
// Returns true only if every check() below succeeded (all are attempted regardless).
static bool applyIdleSettings() {
    bool ok = true;
    ok &= check(sensor.setSensorAltitude(cfg.altitude), "setSensorAltitude()");
    ok &= check(sensor.setVocAlgorithmTuningParameters(cfg.vocIdxOff, cfg.vocLearnOff,
          cfg.vocLearnGain, cfg.vocGateMax, cfg.vocStdInit, cfg.vocGain),
          "setVocAlgorithmTuningParameters()");
    // NOx: learningTimeGainHours must be 12, stdInitial must be 50 (datasheet)
    ok &= check(sensor.setNoxAlgorithmTuningParameters(cfg.noxIdxOff, cfg.noxLearnOff,
          12, cfg.noxGateMax, 50, cfg.noxGain),
          "setNoxAlgorithmTuningParameters()");
    ok &= check(sensor.setCo2SensorAutomaticSelfCalibration(cfg.asc ? 1 : 0),
          "setCo2SensorAutomaticSelfCalibration()");
    return ok;
}

// Commands accepted in any state.
// Returns true only if every check() below succeeded (all are attempted regardless).
static bool applyAnyStateSettings() {
    bool ok = true;
    if (cfg.usePressure)
        ok &= check(sensor.setAmbientPressure(cfg.pressure), "setAmbientPressure()");
    ok &= check(sensor.setTemperatureOffsetParameters(
          (int16_t)lroundf(cfg.tempOffset * 200.0f),
          (int16_t)lroundf(cfg.tempSlope * 10000.0f), 0, 0),
          "setTemperatureOffsetParameters()");
    return ok;
}

static void saveVocState() {
    if (!cfg.vocPersist) return;
    if (state != SensorState::READY && state != SensorState::GAS_CALIBRATION) return;
    uint8_t st[8];
    if (check(sensor.getVocAlgorithmState(st, sizeof st), "getVocAlgorithmState()"))
        prefs.putBytes("vocState", st, sizeof st);
}

// ------------------------------------------------------------
//  Sensor lifecycle
// ------------------------------------------------------------
static bool startSensor() {
    if (!scanBus()) return false;

    sensor.begin(Wire, SEN66_I2C_ADDR_6B);

    // NO deviceReset() on normal boot: it would wipe VOC/NOx learning
    // that survives a stop/start (e.g. after maintenance or a reflash
    // without power loss).
    sensor.stopMeasurement();          // harmless error if already idle
    delay(1500);

    // Volatile settings -> reapplied every boot. Boot continues regardless of
    // whether the sensor accepts them, so results are intentionally ignored here.
    (void)applyIdleSettings();
    (void)applyAnyStateSettings();

    if (cfg.vocPersist && prefs.getBytesLength("vocState") == 8) {
        uint8_t st[8];
        prefs.getBytes("vocState", st, sizeof st);
        if (check(sensor.setVocAlgorithmState(st, sizeof st), "setVocAlgorithmState()"))
            Serial.println("VOC learning state restored.");
    }

    if (!check(sensor.startContinuousMeasurement(), "startContinuousMeasurement()"))
        return false;

    measStartMs = millis();
    Serial.println("Measurement started.");
    return true;
}

// ------------------------------------------------------------
//  Scheduled maintenance (NOT automatic on SEN6x)
// ------------------------------------------------------------
static void doFanCleaning() {
    if (check(sensor.startFanCleaning(), "startFanCleaning()")) {
        delay(12000);                  // datasheet: 10 s + margin
        time_t now = nowEpoch();
        if (now) prefs.putLong64("lastFan", (int64_t)now);
        Serial.println("Fan cleaning done.");
    }
}

static void doShtHeater() {
    if (check(sensor.activateShtHeater(), "activateShtHeater()")) {
        delay(20000);                  // datasheet: wait 20 s before measuring
        time_t now = nowEpoch();
        if (now) prefs.putLong64("lastHeat", (int64_t)now);
        Serial.println("SHT heater cycle done.");
    }
}

static void pauseMeasurement() {
    sensor.stopMeasurement();
    delay(1500);
}

static void resumeMeasurement() {
    if (check(sensor.startContinuousMeasurement(), "restart measurement"))
        measStartMs = millis();        // PM warmup restarts; state machine follows
    else
        publishState(SensorState::FAULT);
}

static void runMaintenanceIfDue() {
    time_t now = nowEpoch();
    if (now == 0) {                    // clock not synced: postpone, never guess
        Serial.println("Maintenance skipped: clock not synced yet.");
        return;
    }
    bool fanDue  = (now - (time_t)prefs.getLong64("lastFan",  0)) > FAN_CLEAN_PERIOD_S;
    bool heatDue = (now - (time_t)prefs.getLong64("lastHeat", 0)) > SHT_HEAT_PERIOD_S;
    if (!fanDue && !heatDue) return;

    publishStatusText("Maintenance...");
    pauseMeasurement();
    if (fanDue)  doFanCleaning();
    if (heatDue) doShtHeater();
    resumeMeasurement();
}

// ------------------------------------------------------------
//  Data publishing
// ------------------------------------------------------------
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

static void handleApiData() {
    String s; s.reserve(300);
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

static void sendErr(int code, const char* msg) {
    String s = "{\"ok\":false,\"error\":\""; s += msg; s += "\"}";
    server.send(code, "application/json", s);
}

static void handleApiSettingsGet() {
    String s; s.reserve(400);
    s += "{\"altitude\":"; s += String(cfg.altitude); s += ',';
    s += "\"usePressure\":"; s += (cfg.usePressure ? "true" : "false"); s += ',';
    s += "\"pressure\":"; s += String(cfg.pressure); s += ',';
    s += "\"tempOffset\":"; s += String(cfg.tempOffset, 3); s += ',';
    s += "\"tempSlope\":"; s += String(cfg.tempSlope, 3); s += ',';
    s += "\"vocPersist\":"; s += (cfg.vocPersist ? "true" : "false"); s += ',';
    s += "\"asc\":"; s += (cfg.asc ? "true" : "false"); s += ',';
    s += "\"frcTarget\":"; s += String(cfg.frcTarget); s += ',';
    s += "\"vocIdxOff\":"; s += String(cfg.vocIdxOff); s += ',';
    s += "\"vocLearnOff\":"; s += String(cfg.vocLearnOff); s += ',';
    s += "\"vocLearnGain\":"; s += String(cfg.vocLearnGain); s += ',';
    s += "\"vocGateMax\":"; s += String(cfg.vocGateMax); s += ',';
    s += "\"vocStdInit\":"; s += String(cfg.vocStdInit); s += ',';
    s += "\"vocGain\":"; s += String(cfg.vocGain); s += ',';
    s += "\"noxIdxOff\":"; s += String(cfg.noxIdxOff); s += ',';
    s += "\"noxLearnOff\":"; s += String(cfg.noxLearnOff); s += ',';
    s += "\"noxGateMax\":"; s += String(cfg.noxGateMax); s += ',';
    s += "\"noxGain\":"; s += String(cfg.noxGain);
    s += '}';
    server.send(200, "application/json", s);
}

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
    // Toggling the ambient-pressure override must also go through the idle
    // path so setSensorAltitude is re-applied (the SEN66 keeps using the
    // last ambient pressure otherwise).
    bool needIdle = c.altitude != cfg.altitude || c.asc != cfg.asc
        || c.vocIdxOff != cfg.vocIdxOff || c.vocLearnOff != cfg.vocLearnOff
        || c.vocLearnGain != cfg.vocLearnGain || c.vocGateMax != cfg.vocGateMax
        || c.vocStdInit != cfg.vocStdInit || c.vocGain != cfg.vocGain
        || c.noxIdxOff != cfg.noxIdxOff || c.noxLearnOff != cfg.noxLearnOff
        || c.noxGateMax != cfg.noxGateMax || c.noxGain != cfg.noxGain
        || c.usePressure != cfg.usePressure;
    bool persistTurnedOff = cfg.vocPersist && !c.vocPersist;

    cfg = c;
    saveSettings();
    if (persistTurnedOff) prefs.remove("vocState");   // key defined in Task 4
    bool applied;
    if (needIdle) {
        pauseMeasurement();
        applied = applyIdleSettings();
        applied &= applyAnyStateSettings();
        resumeMeasurement();          // PM warmup restarts, state machine follows
    } else {
        applied = applyAnyStateSettings();
    }
    if (!applied) { sendErr(500, "sensor rejected settings"); return; }
    server.send(200, "application/json", "{\"ok\":true}");
}

static void sendOk() { server.send(200, "application/json", "{\"ok\":true}"); }

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
        // Reset only VOC/NOx tuning to datasheet defaults; site config
        // (altitude, pressure, temperature offset, ASC, FRC target) is kept.
        Settings fresh;
        cfg.vocIdxOff  = fresh.vocIdxOff;  cfg.vocLearnOff = fresh.vocLearnOff;
        cfg.vocLearnGain = fresh.vocLearnGain; cfg.vocGateMax = fresh.vocGateMax;
        cfg.vocStdInit = fresh.vocStdInit; cfg.vocGain = fresh.vocGain;
        cfg.noxIdxOff  = fresh.noxIdxOff;  cfg.noxLearnOff = fresh.noxLearnOff;
        cfg.noxGateMax = fresh.noxGateMax; cfg.noxGain = fresh.noxGain;
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

void sendData() {
    if (state == SensorState::BOOT || state == SensorState::FAULT) {
        publishState(SensorState::FAULT);
        Serial.println(">>> Sensor not active, retrying...");
        if (startSensor()) publishState(SensorState::WARMUP);
        return;
    }

    float pm1 = 0, pm2p5 = 0, pm4 = 0, pm10 = 0;
    float humidity = 0, temperature = 0;
    float voc = 0, nox = 0;
    uint16_t co2 = 0;

    if (!check(sensor.readMeasuredValues(pm1, pm2p5, pm4, pm10,
                                         humidity, temperature,
                                         voc, nox, co2),
               "readMeasuredValues()")) {
        publishState(SensorState::FAULT);
        return;
    }

    uint32_t age = millis() - measStartMs;

    if (plausible(temperature, -50, 100)) { latest.temp = temperature; histAdd(M_TEMP, temperature); }
    if (plausible(humidity, 0, 100))      { latest.hum  = humidity;   histAdd(M_HUM, humidity); }

    if (age >= WARMUP_PM_MS) {         // PM muted during its 30 s warmup
        if (plausible(pm1,   0, 1000)) { latest.pm1  = pm1;   histAdd(M_PM1,  pm1); }
        if (plausible(pm2p5, 0, 1000)) { latest.pm25 = pm2p5; histAdd(M_PM25, pm2p5); }
        if (plausible(pm4,   0, 1000)) { latest.pm4  = pm4;   histAdd(M_PM4,  pm4); }
        if (plausible(pm10,  0, 1000)) { latest.pm10 = pm10;  histAdd(M_PM10, pm10); }
    }

    if (co2 != 0xFFFF && co2 <= 40000) { latest.co2 = co2; histAdd(M_CO2, co2); }

    if (plausible(voc, 1, 500)) { latest.voc = voc; histAdd(M_VOC, voc); }
    if (plausible(nox, 1, 500)) { latest.nox = nox; histAdd(M_NOX, nox); }

    latest.updatedAt = millis();

    SEN66DeviceStatus ds;
    if (sensor.readDeviceStatus(ds) == NO_ERROR) deviceStatusRaw = ds.value;

    if      (age < WARMUP_PM_MS)  publishState(SensorState::WARMUP);
    else if (age < WARMUP_NOX_MS) publishState(SensorState::GAS_CALIBRATION);
    else                          publishState(SensorState::READY);

    Serial.printf("[%s] CO2 %u | T %.1f | RH %.1f | PM2.5 %.1f | VOC %.1f | NOx %.1f\n",
                  stateText(state), co2, temperature, humidity, pm2p5, voc, nox);
}

// ------------------------------------------------------------
//  Setup / loop
// ------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();

    prefs.begin("airsensor", false);
    loadSettings();
    Wire.begin(PIN_SDA, PIN_SCL, I2C_FREQ);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print('.'); }
    Serial.printf("\nConnected: %s\n", WiFi.localIP().toString().c_str());
    if (MDNS.begin("airsensor")) MDNS.addService("http", "tcp", 80);

    server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", DASHBOARD_HTML); });
    server.on("/admin", HTTP_GET, []() { server.send_P(200, "text/html", ADMIN_HTML); });
    server.on("/api/data", HTTP_GET, handleApiData);
    server.on("/api/history", HTTP_GET, handleApiHistory);
    server.on("/api/settings", HTTP_GET, handleApiSettingsGet);
    server.on("/api/settings", HTTP_POST, handleApiSettingsPost);
    server.on("/api/cmd", HTTP_POST, handleApiCmd);
    server.on("/api/device", HTTP_GET, handleApiDevice);
    server.on("/manifest.json", HTTP_GET, []() { server.send_P(200, "application/manifest+json", MANIFEST_JSON); });
    server.on("/icon.svg", HTTP_GET, []() { server.send_P(200, "image/svg+xml", ICON_SVG); });
    server.onNotFound([]() { server.sendHeader("Location", "/"); server.send(302); });
    server.begin();

    configTime(0, 0, "pool.ntp.org");

    bucketStartMs = millis();
    publishState(SensorState::BOOT);
    if (startSensor()) publishState(SensorState::WARMUP);
}

void loop() {
    server.handleClient();
    histTick();
    static uint32_t tSend = 0, tMaint = 0, tVoc = 0;
    uint32_t now = millis();
    if (now - tSend  >= SEND_INTERVAL_MS) { tSend  = now; sendData(); }
    if (now - tMaint >= MAINT_CHECK_MS)   { tMaint = now; runMaintenanceIfDue(); }
    if (now - tVoc   >= VOC_SAVE_MS)      { tVoc   = now; saveVocState(); }
}