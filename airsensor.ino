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

static constexpr long     SEND_INTERVAL_MS   = 10000;
static constexpr long     MAINT_CHECK_MS     = 60UL * 60 * 1000;
static constexpr uint32_t WARMUP_PM_MS       = 30UL * 1000;
static constexpr uint32_t WARMUP_NOX_MS      = 6UL * 60 * 60 * 1000;
static constexpr uint32_t MIN_RUN_BEFORE_FRC = 3UL * 60 * 1000;
static constexpr time_t   FAN_CLEAN_PERIOD_S = 7UL  * 24 * 3600;   // weekly
static constexpr time_t   SHT_HEAT_PERIOD_S  = 30UL * 24 * 3600;   // monthly
static constexpr time_t   TIME_VALID_AFTER   = 1700000000;

enum class SensorState { BOOT, WARMUP, GAS_CALIBRATION, READY, FAULT };

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

Preferences prefs;                     // stores ONLY the two maintenance dates

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
    s += "\"faultFlags\":"; s += String(deviceStatusRaw); s += ',';
    s += "\"room\":\"Living room\"}";     // Task 3 lo rende configurabile
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

    if (plausible(temperature, -50, 100)) latest.temp = temperature;
    if (plausible(humidity, 0, 100))      latest.hum  = humidity;

    if (age >= WARMUP_PM_MS) {         // PM muted during its 30 s warmup
        if (plausible(pm1,   0, 1000)) latest.pm1  = pm1;
        if (plausible(pm2p5, 0, 1000)) latest.pm25 = pm2p5;
        if (plausible(pm4,   0, 1000)) latest.pm4  = pm4;
        if (plausible(pm10,  0, 1000)) latest.pm10 = pm10;
    }

    if (co2 != 0xFFFF && co2 <= 40000) latest.co2 = co2;

    if (plausible(voc, 1, 500)) latest.voc = voc;
    if (plausible(nox, 1, 500)) latest.nox = nox;

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
    Wire.begin(PIN_SDA, PIN_SCL, I2C_FREQ);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print('.'); }
    Serial.printf("\nConnected: %s\n", WiFi.localIP().toString().c_str());
    if (MDNS.begin("airsensor")) MDNS.addService("http", "tcp", 80);

    server.on("/api/data", HTTP_GET, handleApiData);
    server.begin();

    configTime(0, 0, "pool.ntp.org");

    publishState(SensorState::BOOT);
    if (startSensor()) publishState(SensorState::WARMUP);
}

void loop() {
    server.handleClient();
    static uint32_t tSend = 0, tMaint = 0;
    uint32_t now = millis();
    if (now - tSend  >= SEND_INTERVAL_MS) { tSend  = now; sendData(); }
    if (now - tMaint >= MAINT_CHECK_MS)   { tMaint = now; runMaintenanceIfDue(); }
}