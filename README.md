# Air Sensor

A local, cloud-free air-quality dashboard built around the Sensirion SEN66
sensor module and an ESP32. The ESP32 reads CO2, PM1/PM2.5/PM4/PM10,
VOC index, NOx index, temperature and humidity from the SEN66 over I2C and
serves a live dashboard plus an admin/calibration page directly from its own
web server. There is no cloud service, no external account and no data
leaves the local network.

## Hardware

- ESP32 dev board
- Sensirion SEN66 sensor module, connected over I2C:
  - SDA -> GPIO 22 (green wire)
  - SCL -> GPIO 23 (yellow wire)
  - I2C address: `0x6B`

## Building / flashing

### arduino-cli

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 .
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 .
```

Required library: **Sensirion I2C SEN66** (install via `arduino-cli lib
install "Sensirion I2C SEN66"` or the Arduino IDE Library Manager).
The `esp32` board package (`esp32:esp32`) must also be installed
(`arduino-cli core install esp32:esp32`).

### Arduino IDE

Open `airsensor.ino`, select an ESP32 board (Tools > Board > esp32 > your
board), install the **Sensirion I2C SEN66** library via
Sketch > Include Library > Manage Libraries, then Upload as usual.

Before flashing, set your WiFi credentials at the top of `airsensor.ino`
(`ssid` / `pass`).

## UI development workflow

The dashboard and admin pages are plain HTML/CSS/JS files under `dev/`,
embedded into the firmware as PROGMEM string headers so they can be served
without a filesystem.

1. Run the mock server to iterate on the UI without hardware:
   ```bash
   python3 dev/mock_server.py
   ```
   Serves the same routes as the firmware (`/`, `/admin`, `/api/*`) with
   fake data on `http://localhost:8000`.
2. Edit `dev/dashboard.html` and/or `dev/admin.html`.
3. Regenerate the embedded headers:
   ```bash
   python3 dev/embed.py
   ```
   This writes `dashboard_html.h` and `admin_html.h`. These are
   auto-generated - do not edit them directly.
4. Recompile/reflash the sketch.

## Access

Once connected to WiFi, the device is reachable at:

- `http://airsensor.local` (or its LAN IP) - live dashboard
- `http://airsensor.local/admin` - sensor status, calibration and settings

### Add to Home Screen (iPhone)

The dashboard is installable as a home-screen web app: open it in Safari,
tap Share > Add to Home Screen. It launches full-screen (standalone mode,
no browser chrome). This is a plain installable web page, not a push-capable
PWA - there are no notifications, since the device isn't reachable from
outside the LAN.

## Security note

There is no authentication on any route. Anyone on the local network can
view sensor data and change settings. Only run this on a trusted home/LAN
network, never expose it to the internet. WiFi credentials are stored in
plain text in `airsensor.ino` - keep the source private if that's a
concern.
