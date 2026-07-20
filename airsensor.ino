// ============================================================
//  Air Quality Monitor - SEN66 + ESP32 + Blynk
//  These 4 lines MUST come before the #includes
// ============================================================
#define BLYNK_TEMPLATE_ID   "TMPL4BtE0B-t1"
#define BLYNK_TEMPLATE_NAME "Air Sensor"
#define BLYNK_AUTH_TOKEN    "Xs6wLLCdq7YWOktyuCa7HdREz7iOfIm-"
#define BLYNK_PRINT Serial

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <time.h>
#include <Preferences.h>
#include <BlynkSimpleEsp32.h>
#include <SensirionI2cSen66.h>

#ifdef NO_ERROR
#undef NO_ERROR
#endif
#define NO_ERROR 0

// ============================================================
//  CONFIGURATION - the only section you should ever touch
// ============================================================
char ssid[] = "NETGEAR30";
char pass[] = "TyjH39hFzd";

static constexpr uint16_t SENSOR_ALTITUDE_M = 120;    // your site; never reset
static constexpr uint16_t FRESH_AIR_CO2_PPM = 420;    // reference for forced recalibration

static constexpr int PIN_SDA = 22;   // green wire
static constexpr int PIN_SCL = 23;   // yellow wire

// ============================================================
//  Internals
// ============================================================
static constexpr uint32_t I2C_FREQ = 100000;

static constexpr int VP_TEMP   = V0;
static constexpr int VP_HUM    = V1;
static constexpr int VP_PM1    = V2;
static constexpr int VP_PM25   = V3;
static constexpr int VP_PM4    = V4;
static constexpr int VP_PM10   = V5;
static constexpr int VP_CO2    = V6;
static constexpr int VP_VOC    = V7;
static constexpr int VP_NOX    = V8;
static constexpr int VP_STATUS = V9;
// V10 = reset learning, V11 = fan clean now, V12 = CO2 recal

static constexpr long     SEND_INTERVAL_MS   = 10000;
static constexpr long     MAINT_CHECK_MS     = 60UL * 60 * 1000;
static constexpr uint32_t WARMUP_PM_MS       = 30UL * 1000;
static constexpr uint32_t WARMUP_NOX_MS      = 6UL * 60 * 60 * 1000;
static constexpr uint32_t MIN_RUN_BEFORE_FRC = 3UL * 60 * 1000;
static constexpr time_t   FAN_CLEAN_PERIOD_S = 7UL  * 24 * 3600;   // weekly
static constexpr time_t   SHT_HEAT_PERIOD_S  = 30UL * 24 * 3600;   // monthly
static constexpr time_t   TIME_VALID_AFTER   = 1700000000;

// ---- Threshold colors (Blynk palette) ----
static constexpr const char* COL_GREEN = "#23C48E";
static constexpr const char* COL_AMBER = "#ED9D00";
static constexpr const char* COL_RED   = "#D3435C";

// ---- Alert thresholds (with hysteresis) ----
// ON  = fires above this (matches the red color boundary)
// OFF = rearms below this; the gap is what stops the alert from flapping
static constexpr float CO2_ALERT_ON   = 1400;
static constexpr float CO2_ALERT_OFF  = 1200;
static constexpr float PM25_ALERT_ON  = 35;
static constexpr float PM25_ALERT_OFF = 25;
static constexpr float VOC_ALERT_ON   = 250;
static constexpr float VOC_ALERT_OFF  = 200;
static constexpr float NOX_ALERT_ON   = 100;
static constexpr float NOX_ALERT_OFF  = 50;

enum class SensorState { BOOT, WARMUP, GAS_CALIBRATION, READY, FAULT };

SensirionI2cSen66 sensor;
BlynkTimer timer;
Preferences prefs;                     // stores ONLY the two maintenance dates

static char errorMessage[128];
static SensorState state     = SensorState::BOOT;
static SensorState statePrev = SensorState::FAULT;
static uint32_t measStartMs  = 0;

// Remembers the current color per pin so we only write on change
// (setProperty must never be spammed, per Blynk docs)
static const char* lastCo2Col  = nullptr;
static const char* lastPm25Col = nullptr;
static const char* lastVocCol  = nullptr;

// One latch per alert: true while the episode is ongoing
static bool co2AlertActive  = false;
static bool pm25AlertActive = false;
static bool vocAlertActive  = false;
static bool noxAlertActive  = false;

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

// Fires the event once per episode; rearms only after the value
// drops back under offThreshold. Without the gap between the two
// thresholds a value hovering on the edge would burn the
// 100-events-per-day quota in minutes.
static void checkAlert(float value, float onThreshold, float offThreshold,
                       bool* active, const char* eventCode) {
    if (!*active && value >= onThreshold) {
        Blynk.logEvent(eventCode);
        *active = true;
    } else if (*active && value <= offThreshold) {
        *active = false;
    }
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
        Blynk.virtualWrite(VP_STATUS, stateText(s));
        Blynk.setProperty(VP_STATUS, "color",
            s == SensorState::READY ? COL_GREEN :
            s == SensorState::FAULT ? COL_RED   :
                                       COL_AMBER);
        Serial.printf(">>> State: %s\n", stateText(s));
        statePrev = s;
    }
}

static void publishStatusText(const char* msg) {   // transient messages on V9
    Blynk.virtualWrite(VP_STATUS, msg);
    Serial.printf(">>> %s\n", msg);
    statePrev = SensorState::FAULT;   // force next publishState() to rewrite V9
}

static void setColorIfChanged(int vpin, const char* col, const char** last) {
    if (col != *last) {
        Blynk.setProperty(vpin, "color", col);
        *last = col;
    }
}

static const char* co2Color(uint16_t v) {
    if (v < 1000) return COL_GREEN;
    if (v < 1400) return COL_AMBER;
    return COL_RED;
}

static const char* pm25Color(float v) {
    if (v < 15)  return COL_GREEN;
    if (v < 35)  return COL_AMBER;
    return COL_RED;
}

static const char* vocColor(float v) {
    if (v < 150) return COL_GREEN;
    if (v < 250) return COL_AMBER;
    return COL_RED;
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

    // Volatile settings -> reapplied every boot:
    check(sensor.setSensorAltitude(SENSOR_ALTITUDE_M), "setSensorAltitude()");
    // CO2 self-calibration: on by default, corrections stored inside
    // the sensor's own persistent memory. Nothing to do.

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
//  Admin controls
// ------------------------------------------------------------
// V10: full learning reset. Wipes VOC/NOx learning + maintenance dates.
// Cannot wipe CO2 calibration (sensor NVM, no erase command exists).
// Altitude is spared: config constant, reapplied at next start.
BLYNK_WRITE(V10) {
    if (param.asInt() != 1) return;
    publishStatusText("Resetting learning...");

    prefs.clear();
    pauseMeasurement();
    check(sensor.deviceReset(), "deviceReset()");   // the ONE legitimate use
    delay(1500);

    publishState(SensorState::BOOT);
    if (startSensor()) publishState(SensorState::WARMUP);
    Blynk.virtualWrite(V10, 0);
}

// V11: fan cleaning on demand
BLYNK_WRITE(V11) {
    if (param.asInt() != 1) return;
    publishStatusText("Cleaning fan...");
    pauseMeasurement();
    doFanCleaning();
    resumeMeasurement();
    Blynk.virtualWrite(V11, 0);
}

// V12: forced CO2 recalibration.
// ONLY with the sensor at an open window / outdoors, after measuring
// there for at least 3 minutes.
BLYNK_WRITE(V12) {
    if (param.asInt() != 1) return;

    if (millis() - measStartMs < MIN_RUN_BEFORE_FRC) {
        publishStatusText("FRC refused: run >= 3 min first");
        Blynk.virtualWrite(V12, 0);
        return;
    }

    publishStatusText("CO2 recalibration...");
    pauseMeasurement();                // datasheet: wait >= 1400 ms before FRC

    uint16_t correction = 0;
    if (check(sensor.performForcedCo2Recalibration(FRESH_AIR_CO2_PPM, correction),
              "performForcedCo2Recalibration()")) {
        if (correction == 0xFFFF) publishStatusText("CO2 recalibration FAILED");
        else                      publishStatusText("CO2 recalibrated OK");
    }
    delay(500);
    resumeMeasurement();
    Blynk.virtualWrite(V12, 0);
}

// ------------------------------------------------------------
//  Data publishing
// ------------------------------------------------------------
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

    if (plausible(temperature, -50, 100)) Blynk.virtualWrite(VP_TEMP, temperature);
    if (plausible(humidity, 0, 100))      Blynk.virtualWrite(VP_HUM,  humidity);

    if (age >= WARMUP_PM_MS) {         // PM muted during its 30 s warmup
        if (plausible(pm1,   0, 1000)) Blynk.virtualWrite(VP_PM1,  pm1);
        if (plausible(pm2p5, 0, 1000)) {
            Blynk.virtualWrite(VP_PM25, pm2p5);
            setColorIfChanged(VP_PM25, pm25Color(pm2p5), &lastPm25Col);
            checkAlert(pm2p5, PM25_ALERT_ON, PM25_ALERT_OFF, &pm25AlertActive, "pm_high");
        }
        if (plausible(pm4,   0, 1000)) Blynk.virtualWrite(VP_PM4,  pm4);
        if (plausible(pm10,  0, 1000)) Blynk.virtualWrite(VP_PM10, pm10);
    }

    if (co2 != 0xFFFF && co2 <= 40000) {
        Blynk.virtualWrite(VP_CO2, co2);
        setColorIfChanged(VP_CO2, co2Color(co2), &lastCo2Col);
        checkAlert(co2, CO2_ALERT_ON, CO2_ALERT_OFF, &co2AlertActive, "co2_high");
    }

    if (plausible(voc, 1, 500)) {
        Blynk.virtualWrite(VP_VOC, voc);
        setColorIfChanged(VP_VOC, vocColor(voc), &lastVocCol);
        checkAlert(voc, VOC_ALERT_ON, VOC_ALERT_OFF, &vocAlertActive, "voc_high");
    }

    if (plausible(nox, 1, 500)) {
        Blynk.virtualWrite(VP_NOX, nox);
        checkAlert(nox, NOX_ALERT_ON, NOX_ALERT_OFF, &noxAlertActive, "nox_high");
    }

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
    Wire.begin(PIN_SDA, PIN_SCL, I2C_FREQ);

    Serial.println("Connecting to WiFi and Blynk...");
    Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);
    Serial.println("Connected.");

    configTime(0, 0, "pool.ntp.org");

    publishState(SensorState::BOOT);
    if (startSensor()) publishState(SensorState::WARMUP);

    timer.setInterval(SEND_INTERVAL_MS, sendData);
    timer.setInterval(MAINT_CHECK_MS,   runMaintenanceIfDue);
}

void loop() {
    Blynk.run();
    timer.run();
}