# AirCube

A compact desk air-quality monitor built around the **ESP32-H2**. It measures temperature, humidity, eCO2, eTVOC and AQI, shows the air quality as a glowing LED (green = good, red = bad), and reports every value to your smart home over **Zigbee**.

This repository contains everything needed to build one: firmware, PCB manufacturing files, and a 3D-printable enclosure.

## Features

- **ENS161** air-quality sensor → eCO2 (ppm), eTVOC (ppb), AQI index
- **ENS210** temperature + humidity sensor (also used to compensate the ENS161 for accurate readings)
- **3× WS2812B NeoPixels** with smooth color transitions, AQI → green/red gradient
- **Zigbee end device** — works with Zigbee2MQTT / ZHA / Home Assistant, exposes 4 endpoints
- Single button: short press cycles LED brightness (persisted), 3 s long press = Zigbee factory reset / re-pair
- USB-C power, 3.3 V + 1.8 V LDOs, ESD protection
- Two-piece 3D-printed enclosure (`top.stl` / `bottom.stl`)

## Repository layout

```
aircube/
├── pcb/                    PCB manufacturing files
│   ├── Schematic.pdf       circuit schematic
│   ├── Gerber.zip          fabrication data
│   ├── BOM.csv             bill of materials (LCSC part numbers)
│   └── PickAndPlace.csv    SMT placement file
├── 3d printing/
│   ├── top.stl             enclosure top
│   └── bottom.stl          enclosure bottom
└── firmware/
    └── aircube.ino         firmware (single Arduino sketch)
```

## Hardware

| Part | Role |
| --- | --- |
| ESP32-H2-MINI-1-N4 | MCU with integrated 802.15.4 / Zigbee radio |
| ScioSense ENS161 | Air-quality sensor (eCO2, eTVOC, AQI) |
| ScioSense ENS210 | Temperature / humidity sensor |
| WS2812B-2020 ×3 | RGB status LED |
| AP2120N-3.3 / AP2120N-1.8 | 3.3 V and 1.8 V regulators |
| USB-C (16-pin) | Power input |
| TPD1E10B06DPYR | USB ESD protection |
| BSS138 | I2C level shifter |
| Tactile switch | Brightness / pairing button |

Full part numbers and quantities are in [`pcb/BOM.csv`](pcb/BOM.csv).

### Pin assignment

| Function | GPIO |
| --- | --- |
| I2C SCL | 0 |
| I2C SDA | 1 |
| Button | 11 |
| NeoPixel data | 25 |

## Firmware

Single sketch: [`firmware/aircube.ino`](firmware/aircube.ino), built on FreeRTOS tasks.

### Requirements

- **Arduino-ESP32 core** with Zigbee support
- Board: `ESP32-H2 Dev Module`
- Tools → **Zigbee mode: ED** (end device) — the build aborts with an `#error` if this is not set
- Libraries:
  - Adafruit NeoPixel
  - ArduinoJson
  - ScioSense ENS210
  - ScioSense ENS16x

### Task structure

| Task | Rate | Purpose |
| --- | --- | --- |
| `sensor_task` | 1 s | Reads both sensors, prints JSON to serial, reports to Zigbee |
| `led_task` | 20 ms | Applies color/brightness to the NeoPixels |
| `button_task` | event-driven | Debounced short/long press handling |
| `loop()` | 20 ms | Smooth hue interpolation toward the target AQI color |

### Zigbee endpoints

| Endpoint | Type | Exposes |
| --- | --- | --- |
| 10 | `ZigbeeTempSensor` | Temperature (°C), Humidity (%) |
| 11 | `ZigbeeCarbonDioxideSensor` | eCO2 (ppm) |
| 12 | `ZigbeeAnalog` | eTVOC (ppb), description `eTVOC` |
| 13 | `ZigbeeAnalog` | AQI index, description `AQI` |

Manufacturer/model: `TinySquare / AirCube`. Reporting interval: 60 s, or sooner when a value changes beyond its threshold.

### Behavior

- **Startup**: rainbow green → red → green animation over 3 s (skipped when booting into pairing mode).
- **LED color**: AQI is clamped to 0–200. AQI ≤ 10 is full green; above that the hue sweeps toward red. Color changes are interpolated at 2 % per frame so they glide.
- **Short press**: cycles brightness 0 → 10 → 30 → 60 → 100 %, stored in NVS.
- **Long press (3 s)**: Zigbee factory reset and reboot into pairing mode (blinking blue LEDs, 60 s timeout).
- **Serial**: 115200 baud, emits one JSON object per second:

  ```json
  {"ens210":{"temperature_c":24.15,"temperature_f":75.47,"humidity":41.20},
   "ens16x":{"status":"OK","etvoc":12,"eco2":419,"aqi":3,"aqi_uba":1},
   "timestamp":12345}
  ```

## Getting started

1. **Order the PCB** — send `pcb/Gerber.zip`, `pcb/BOM.csv` and `pcb/PickAndPlace.csv` to your fab (files are UTF-16 encoded, as exported by the CAD tool).
2. **Print the case** — `3d printing/top.stl` and `3d printing/bottom.stl`.
3. **Flash the firmware** — open `firmware/aircube.ino` in Arduino IDE, select `ESP32-H2 Dev Module`, set Zigbee mode to `ED`, install the libraries above, then upload (enable the bootloader/`BOOT` button if needed).
4. **Pair** — the device joins an existing network automatically, or hold the button for 3 s to force a fresh pairing window (blinking blue).
5. **Integrate** — devices appear in Zigbee2MQTT / Home Assistant as `TinySquare AirCube` endpoints.

## License

No license file is included yet — all rights reserved by default. Add one (e.g. MIT for the firmware, CERN-OHL for the hardware) if you plan to share it.
