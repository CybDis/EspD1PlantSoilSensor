# ESP8266 Plant Soil Moisture Sensor

Battery-powered soil moisture monitor for up to 4 plants. Wakes from deep sleep, reads all sensors, publishes data to Home Assistant via MQTT, then sleeps again. Skips measurements during configurable night hours to save power.

---

## How it works

```
Wake from deep sleep
  |
  +-- Night time? (RTC memory check, no WiFi needed)
  |       YES --> sleep until morning (in chunks up to hardware max ~3h20m)
  |
  NO
  |
  Connect WiFi --> NTP time sync
  |
  Power on sensors (D5 HIGH)
  Read 4x soil moisture via ADS1115
  Read battery voltage via A0
  |
  Publish 7 values to Home Assistant (MQTT Discovery)
  |
  Power off sensors (D5 LOW)
  Store wake time in RTC memory
  Deep sleep:  battery > 80% --> 1 hour
               battery <= 80% --> 3 hours
```

**Night mode** (default 22:00-06:00 local time): the device wakes, checks the RTC memory estimate, and immediately goes back to sleep without connecting to WiFi or powering on sensors.

---

## Home Assistant entities

| Entity | Unit | Device class |
|---|---|---|
| Soil 1-4 | % | moisture |
| Battery Voltage | V | voltage |
| Battery Percent | % | battery |
| Last Updated | - | timestamp |

Devices appear automatically via MQTT Discovery. No manual HA configuration needed.

---

## Hardware

| Component | Description |
|---|---|
| ESP8266 WROOM-02 | Microcontroller (D1-style board) |
| ADS1115 | 16-bit I2C ADC, 4 channels |
| 4x capacitive soil moisture sensors | One per plant |
| LiPo battery | 3.7V nominal, 4.2V max |
| Voltage divider | Scales 4.2V battery to 1.0V for A0 |
| N-channel MOSFET or NPN transistor | Lets D5 switch sensor power rail |

---

## Wiring

### Deep sleep wake-up (required)

```
RST  <----> D0 (GPIO16)
```

Without this connection the device never wakes from deep sleep.  
Hint: Add an on/off-switch inbetween to enable flashing via USB.

### ADS1115 (I2C)

```
ADS1115   ESP8266
--------  -------
VCC    -> 3.3V
GND    -> GND
SDA    -> D2 (GPIO4)
SCL    -> D1 (GPIO5)
ADDR   -> GND          (I2C address 0x48)
A0     -> Soil sensor 1 signal
A1     -> Soil sensor 2 signal
A2     -> Soil sensor 3 signal
A3     -> Soil sensor 4 signal
```

### Sensor power switch (D5)

D5 (GPIO14) drives the VCC line of all 4 soil sensors and the ADS1115 via a switching transistor. When the device sleeps, D5 goes LOW and cuts power to all sensors, eliminating standby current draw.

```
D5 (GPIO14) -> gate/base of switching transistor -> sensor VCC rail
```

### Battery voltage divider (A0)

The ESP8266 ADC input (A0) accepts 0-1V. A resistor divider scales the LiPo voltage (0-4.2V) down:

```
Battery (+) ---[R1: 330k]---+---[R2: 100k]--- GND
                            |
                           A0
```

Ratio: 100k / (330k + 100k) = 0.233  ->  4.2V * 0.233 = 0.977V (within A0 range)

Adjust the `volt * 4.2` factor in `updateSensor()` if you use different resistor values.

### Soil sensor wiring

Each capacitive sensor:
```
Sensor VCC  -> switched VCC rail (controlled by D5 transistor)
Sensor GND  -> GND
Sensor AOUT -> ADS1115 Ax
```

---

## Configuration

Copy `include/user_settings_template.h` to `include/user_settings.h` and fill in your values:

| Setting | Description |
|---|---|
| `MY_NTP_SERVER` | NTP server (default: pool.ntp.org) |
| `MY_TZ` | POSIX timezone string |
| `TIME_TO_SLEEP` | Sleep duration when battery > 80% (seconds) |
| `TIME_TO_SLEEP_LONG` | Sleep duration when battery <= 80% (seconds) |
| `NIGHT_START_HOUR` | Start of no-measurement window (local hour, 0-23) |
| `NIGHT_END_HOUR` | End of no-measurement window (local hour, 0-23) |
| `deviceName` | Name shown in WiFi and Home Assistant |
| `ssid` / `password` | WiFi credentials |
| `mqttBrokerHost` | MQTT broker hostname or IP |
| `mqttUser` / `mqttPass` | MQTT credentials |
| `air0..3` / `water0..3` | Calibration values per sensor (see below) |

---

## Sensor calibration

Each soil sensor needs two reference readings:

- **air value**: ADC reading with the sensor in open air (dry reference)
- **water value**: ADC reading with the sensor submerged in water (wet reference)

To calibrate:

1. Set `doCalibration = true` in `user_settings.h`
2. Flash and open the serial monitor at 115200 baud
3. Hold each sensor in air, note the stable ADC reading -> `airN`
4. Submerge each sensor in water, note the stable ADC reading -> `waterN`
5. Enter the values for `air0..3` and `water0..3` in `user_settings.h`
6. Set `doCalibration = false` and reflash

---

## Build and flash

Built with [PlatformIO](https://platformio.org/).

```bash
# build
pio run

# flash
pio run --target upload

# serial monitor
pio device monitor
```

Dependencies (resolved automatically by PlatformIO):

| Library | Purpose |
|---|---|
| arduino-home-assistant ^2.0.0 | MQTT Discovery for Home Assistant |
| Adafruit ADS1X15 ^2.4.0 | ADS1115 ADC driver |
