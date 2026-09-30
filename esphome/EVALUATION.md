# ESPHome Port — Evaluation & Decision

**Status:** Declined · 2026-09-30 · Branch kept as a record of the research

## Decision

The AirCube stays on the **Arduino firmware** (`firmware/aircube.ino`). No ESPHome
port, and no ESP-IDF rewrite either. The findings below document *why*, and what
to do differently if this is ever revisited.

## Context

The AirCube runs a complete, hardware-proven Arduino firmware: ENS161 + ENS210
sensors, NeoPixel AQI gradient, button UX, and a **Zigbee end device exposing
standard ZCL clusters** (temperature/humidity, CO₂, analog) — discovered by
ZHA/Zigbee2MQTT with zero coordinator-side configuration. The question was
whether a native ESPHome port would be worth the migration.

## Key findings

### What ESPHome *does* support (as of 2026.9)

- Native [`zigbee` component](https://esphome.io/components/zigbee/) for
  ESP32-H2/C6/C5, end-device, entities via Z2M ≥ 2.8 or ZHA — architecture
  matches the current firmware.
- Built-in `ens210` platform ✓ (temp/humidity + `compensation` wiring).

### What ESPHome does *not* support / costs discovered

1. **ENS161 is not supported.** The built-in `ens160` driver hard-fails unless
   `PART_ID == 0x0160`; the board's chip reports `0x0161` → `mark_failed()`.
   A custom component would be required.
2. **Community ENS161 component has bugs.**
   [`kix1979/ens161`](https://github.com/kix1979/ens161) (MIT) validates
   `0x0161` and has the right skeleton, but:
   - writes temperature compensation as `24 * 64` (**°C**×64) where `TEMP_IN`
     expects **Kelvin**×64 — off by 273 K. Confirmed three ways: the ENS210
     library documents its raw `T` as 1/64 K, `aircube.ino` passes
     `getDataT()` straight into `writeCompensation()`, and ESPHome's own driver
     writes `(T + 273.15) * 64`.
   - reads register `0x26` and labels it "formaldehyde"; the current ENS161
     datasheet (v1.1) defines `0x26 = DATA_AQI_S` — the ScioSense 0–200 index.
   - immature: 1 star, 8 commits, last touched Jan 2025.
3. **Entity quality would regress.** ESPHome maps *every* sensor to an
   **Analog Input cluster** → generic entities in HA. The current firmware uses
   proper temperature/humidity/CO₂ clusters that ZHA/Z2M understand natively.
   Matching today's behavior would require writing our own **Z2M external
   converter + ZHA quirk** in addition to the driver.
4. **ESP32-H2 has no Wi-Fi** → no ESPHome native API, no OTA, no web UI,
   USB-only flashing. ESPHome's headline advantages don't apply to this chip.
5. **Config changes require re-pairing / re-interviewing** the device, every
   time.
6. **No brightness entity over Zigbee** — ESPHome only maps
   `sensor`/`binary_sensor` on ESP32 (`number` is nRF52-only). The current
   firmware's brightness reporting stays out of reach without upstream work.
7. The `zigbee` component itself is young (landed 2026.5) → version pinning
   and churn.

### Alternative firmwares evaluated

| Option | Verdict |
| --- | --- |
| **Arduino (current)** | ✅ Done, working, proper HA entities, no coordinator config. One `.ino` to maintain. |
| **ESPHome** | ❌ ~2–4 sessions of custom driver + converter/quirk work to reach *parity*, plus permanent re-pairing friction and a young stack. Real benefit here is mostly YAML aesthetics. |
| **ESP-IDF native** | ❌ Full rewrite of working functionality (esp-zigbee ZCL, NeoPixel, button/NVS, pairing flow) with no upside — Arduino-ESP32 is already a thin layer over ESP-IDF + esp-zigbee, and there's no OTA/API gain on an H2. |

## Consequences

- Development effort goes to **features/polish on `firmware/aircube.ino`**.
- The `esphome` branch is closed for new work; this document is its output.
- ESPHome upstream gaps (ENS161 driver, `number` over Zigbee) remain open
  opportunities if the ecosystem matures enough to revisit.

## If revisited later — starting points

- **Driver:** vendor the official **ScioSense [`ens16x-arduino`](https://github.com/sciosense/ens16x-arduino) (MIT)** —
  it has first-class ENS161 support (`Ens161_IsConnected` checks `0x161`,
  `startStandardMeasure()`, `getAirQualityIndex_ScioSense()`) and is exactly
  what the Arduino firmware runs today. Either as a thin ESPHome wrapper
  (Arduino framework) or as reference for a native `i2c::I2CDevice` component
  modeled on ESPHome's `ens160_base` (GPL-3.0 C++ — attribute accordingly).
- **Coordinator glue:** use StuckAtPrototype/AirCube's
  [`z2m/aircube.mjs`](https://github.com/StuckAtPrototype/AirCube/blob/master/z2m/aircube.mjs)
  and [`zha/aircube.py`](https://github.com/StuckAtPrototype/AirCube/blob/master/zha/aircube.py)
  as templates, retargeted to ESPHome's analog-input clusters.
- **LED scale:** StuckAtPrototype's canonical **VOC Level 0–500** bands
  (TVOC-derived) are a better gradient source than AQI-UBA 1–5; the band table
  and their `HOME_ASSISTANT.md` install pitfalls are worth copying.
- **Skeleton to resume from** (Phase 1 as originally planned):

  ```yaml
  esphome:
    name: aircube
  esp32:
    board: esp32h2
    framework:
      type: esp-idf
  i2c:
    sda: GPIO1
    scl: GPIO0
  zigbee:
    id: my_zigbee
    model: AirCube
  logger:
  ```

## References

- ENS161 datasheet (SC-001855-DS-6 v1.1): registers `0x10 OPMODE`,
  `0x13 TEMP_IN`, `0x15 RH_IN`, `0x20 DEVICE_STATUS`, `0x21 DATA_AQI_UBA`,
  `0x22 DATA_ETVOC`, `0x24 DATA_ECO2`, `0x26 DATA_AQI_S`
- ESPHome: [`zigbee`](https://esphome.io/components/zigbee/),
  [`ens160`](https://esphome.io/components/sensor/ens160/) (source:
  `esphome/components/ens160_base`),
  [`ens210`](https://esphome.io/components/sensor/ens210/)
- [StuckAtPrototype/AirCube](https://github.com/StuckAtPrototype/AirCube) (Apache-2.0)
  — this project's upstream; credit in `NOTICE` where derived material is used
- [kix1979/ens161](https://github.com/kix1979/ens161) (MIT) — reference only
