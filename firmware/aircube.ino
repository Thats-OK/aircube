/**
 * @file aircube.ino
 * @brief AirCube - ESP32-H2 Air Quality Monitor (Arduino Version)
 *
 * Reads ENS210 (temperature/humidity) and ENS161 (air quality) sensors,
 * displays AQI as LED color gradient (green=good, red=bad),
 * and exposes all sensor data via Zigbee Home Automation:
 *   - EP10: Temperature + Humidity (ZigbeeTempSensor)
 *   - EP11: eCO2 in ppm (ZigbeeCarbonDioxideSensor)
 *   - EP12: eTVOC in ppb (ZigbeeAnalog)
 *   - EP13: AQI index (ZigbeeAnalog)
 *
 * Board:  ESP32-H2 Dev Module
 * Zigbee: Zigbee ED (Tools -> Zigbee mode)
 * Libs:   Adafruit NeoPixel, ArduinoJson, ScioSense ENS210, ScioSense ENS16x
 */

#ifndef ZIGBEE_MODE_ED
#error "Zigbee end device mode is not selected in Tools->Zigbee mode"
#endif

// ── Includes ────────────────────────────────────────────────────────────

#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <math.h>

#include "ens210.h"
#include <ScioSense_ENS16x.h>

#include "Zigbee.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

// ── Pin Definitions ─────────────────────────────────────────────────────

#define PIN_I2C_SCL       0
#define PIN_I2C_SDA       1
#define PIN_BUTTON       11
#define PIN_LED_DATA     25

// ── LED Constants ───────────────────────────────────────────────────────

#define NUM_LEDS          5
#define NUM_CONTROLLED    3
#define LED_COLOR_OFF    0x000000
#define LED_COLOR_BLUE   0x0000FF

// ── AQI Color Mapping ───────────────────────────────────────────────────

#define AQI_MIN              0
#define AQI_MAX            200
#define AQI_GREEN_THRESHOLD 10
#define HUE_GREEN        21845
#define TRANSITION_SPEED  0.02f
#define MAX_BRIGHTNESS    1.0f

// ── Button Constants ────────────────────────────────────────────────────

#define DEBOUNCE_MS      50
#define LONG_PRESS_MS  3000

// ── Sensor Constants ────────────────────────────────────────────────────

#define ENS16X_I2C_ADDR  0x52
#define TEMP_OFFSET      -2.0f

// ── Zigbee Constants ────────────────────────────────────────────────────

#define EP_TEMP_HUM      10
#define EP_CO2           11
#define EP_TVOC          12
#define EP_AQI           13
#define PAIRING_TIMEOUT_MS 60000

// ── Global Objects ──────────────────────────────────────────────────────

Adafruit_NeoPixel strip(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);
Preferences preferences;

// Zigbee endpoints
ZigbeeTempSensor          zbTempHum(EP_TEMP_HUM);
ZigbeeCarbonDioxideSensor zbCO2(EP_CO2);
ZigbeeAnalog              zbTVOC(EP_TVOC);
ZigbeeAnalog              zbAQI(EP_AQI);

// Sensor objects (created in setup, after Zigbee)
ENS210 *pEns210 = nullptr;
ENS161 *pEns16x = nullptr;

// ── FreeRTOS Handles ────────────────────────────────────────────────────

static SemaphoreHandle_t led_mutex = NULL;
static QueueHandle_t     gpio_evt_queue = NULL;

// ── LED State ───────────────────────────────────────────────────────────

static uint32_t led_color_grb = LED_COLOR_OFF;
static float    led_intensity = 0.6f;

// ── Brightness Levels ───────────────────────────────────────────────────

static const float brightness_levels[] = {0.0f, 0.1f, 0.3f, 0.6f, 1.0f};
static const int   num_brightness_levels = sizeof(brightness_levels) / sizeof(brightness_levels[0]);
static int         current_brightness_index = 3;

// ── Sensor Data (shared between tasks) ──────────────────────────────────

static volatile int current_aqi = 0;

// ── Smooth Transition State ─────────────────────────────────────────────

static float    current_hue = 21845.0f;
static uint16_t target_hue  = 21845;

// ── Zigbee Pairing State ────────────────────────────────────────────────

static volatile bool zigbee_pairing_active = false;
static unsigned long pairing_start_ms = 0;

// =====================================================================
//  LED Color Library
// =====================================================================

static void hue_to_rgb(float h, float *r, float *g, float *b) {
    float x = 1.0f - fabsf(fmodf(h * 6.0f, 2.0f) - 1.0f);
    if      (h < 1.0f/6.0f) { *r = 1; *g = x; *b = 0; }
    else if (h < 2.0f/6.0f) { *r = x; *g = 1; *b = 0; }
    else if (h < 3.0f/6.0f) { *r = 0; *g = 1; *b = x; }
    else if (h < 4.0f/6.0f) { *r = 0; *g = x; *b = 1; }
    else if (h < 5.0f/6.0f) { *r = x; *g = 0; *b = 1; }
    else                     { *r = 1; *g = 0; *b = x; }
}

static uint32_t get_color_from_hue(uint16_t hue) {
    float h = hue / 65536.0f;
    float r, g, b;
    hue_to_rgb(h, &r, &g, &b);
    r *= MAX_BRIGHTNESS * 255.0f;
    g *= MAX_BRIGHTNESS * 255.0f;
    b *= MAX_BRIGHTNESS * 255.0f;
    return ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b;
}

static uint32_t apply_color_intensity(uint32_t color, float intensity) {
    if (intensity < 0.0f) intensity = 0.0f;
    if (intensity > 1.0f) intensity = 1.0f;
    uint8_t g = (color >> 16) & 0xFF;
    uint8_t r = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    return ((uint32_t)(g * intensity + 0.5f) << 16) |
           ((uint32_t)(r * intensity + 0.5f) << 8) |
            (uint32_t)(b * intensity + 0.5f);
}

static uint16_t aqi_to_hue(int aqi) {
    if (aqi < AQI_MIN) aqi = AQI_MIN;
    if (aqi > AQI_MAX) aqi = AQI_MAX;
    if (aqi <= AQI_GREEN_THRESHOLD) return HUE_GREEN;
    float ratio = (float)(aqi - AQI_GREEN_THRESHOLD) / (float)(AQI_MAX - AQI_GREEN_THRESHOLD);
    return HUE_GREEN - (uint16_t)(ratio * HUE_GREEN);
}

// =====================================================================
//  LED Control
// =====================================================================

static void led_set_color(uint32_t color_grb) {
    if (led_mutex && xSemaphoreTake(led_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        led_color_grb = color_grb;
        xSemaphoreGive(led_mutex);
    }
}

static void led_set_intensity(float intensity) {
    if (intensity < 0.0f) intensity = 0.0f;
    if (intensity > 1.0f) intensity = 1.0f;
    if (led_mutex && xSemaphoreTake(led_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        led_intensity = intensity;
        xSemaphoreGive(led_mutex);
    }
}

static void led_task(void *pvParameters) {
    while (1) {
        uint32_t color = LED_COLOR_OFF;
        float intensity = 0.0f;

        if (led_mutex && xSemaphoreTake(led_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            color = led_color_grb;
            intensity = led_intensity;
            xSemaphoreGive(led_mutex);
        }

        uint32_t final_grb = apply_color_intensity(color, intensity);
        uint8_t g = (final_grb >> 16) & 0xFF;
        uint8_t r = (final_grb >> 8) & 0xFF;
        uint8_t b = final_grb & 0xFF;

        for (int i = 0; i < NUM_CONTROLLED; i++) {
            strip.setPixelColor(i, strip.Color(r, g, b));
        }
        for (int i = NUM_CONTROLLED; i < NUM_LEDS; i++) {
            strip.setPixelColor(i, 0);
        }
        strip.show();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// =====================================================================
//  Button
// =====================================================================

static void IRAM_ATTR button_isr_handler(void *arg) {
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void button_task(void *pvParameters) {
    uint32_t io_num;
    TickType_t last_press_time = 0;

    while (1) {
        if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            TickType_t now = xTaskGetTickCount();
            if (now - last_press_time < pdMS_TO_TICKS(DEBOUNCE_MS)) continue;

            if (digitalRead(PIN_BUTTON) == HIGH) {
                last_press_time = now;
                TickType_t press_start = xTaskGetTickCount();
                bool long_press = false;

                while (digitalRead(PIN_BUTTON) == HIGH) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                    if ((xTaskGetTickCount() - press_start) >= pdMS_TO_TICKS(LONG_PRESS_MS)) {
                        long_press = true;
                        break;
                    }
                }

                if (long_press) {
                    Serial.println("[button] Long press - Zigbee factory reset");
                    preferences.begin("aircube", false);
                    preferences.putBool("zb_pairing", true);
                    preferences.end();
                    Zigbee.factoryReset();
                    while (digitalRead(PIN_BUTTON) == HIGH) vTaskDelay(pdMS_TO_TICKS(50));
                    while (xQueueReceive(gpio_evt_queue, &io_num, 0) == pdTRUE) {}
                } else {
                    current_brightness_index = (current_brightness_index + 1) % num_brightness_levels;
                    float new_brightness = brightness_levels[current_brightness_index];
                    led_set_intensity(new_brightness);
                    preferences.begin("aircube", false);
                    preferences.putInt("led_bright", current_brightness_index);
                    preferences.end();
                    Serial.printf("[button] Brightness: %.1f\n", new_brightness);
                }
            }
        }
    }
}

// =====================================================================
//  Sensor Task
// =====================================================================

static void sensor_task(void *pvParameters) {
    Serial.println("[sensor] Task started");

    while (1) {
        float temp_c = 0.0f;
        float humidity = 0.0f;

        if (pEns210 && pEns210->singleShotMeasure() == RESULT_OK) {
            temp_c = pEns210->getTempCelsius() + TEMP_OFFSET;
            humidity = pEns210->getHumidityPercent();
            // Compensate ENS161 with ENS210 temperature/humidity for more accurate readings
            if (pEns16x) {
                pEns16x->writeCompensation(pEns210->getDataT(), pEns210->getDataH());
            }
        }

        int etvoc = 0, eco2 = 0, aqi = 0, aqi_uba = 0;
        const char *ens16x_status_str = "Unknown";

        if (pEns16x) {
            pEns16x->wait();
            if (pEns16x->update() == RESULT_OK) {
                if (pEns16x->hasNewData()) {
                    etvoc = pEns16x->getTvoc();
                    eco2 = pEns16x->getEco2();
                    aqi = (int)pEns16x->getAirQualityIndex_ScioSense();
                    aqi_uba = (int)pEns16x->getAirQualityIndex_UBA();
                    ens16x_status_str = "OK";
                } else {
                    ens16x_status_str = "No New Data";
                }
            } else {
                ens16x_status_str = "Read Error";
            }
        }

        current_aqi = aqi;

        // JSON Serial Output
        JsonDocument doc;
        JsonObject ens210_obj = doc["ens210"].to<JsonObject>();
        ens210_obj["temperature_c"] = serialized(String(temp_c, 2));
        ens210_obj["temperature_f"] = serialized(String(temp_c * 9.0f / 5.0f + 32.0f, 2));
        ens210_obj["humidity"] = serialized(String(humidity, 2));

        JsonObject ens16x_obj = doc["ens16x"].to<JsonObject>();
        ens16x_obj["status"] = ens16x_status_str;
        ens16x_obj["etvoc"] = etvoc;
        ens16x_obj["eco2"] = eco2;
        ens16x_obj["aqi"] = aqi;
        ens16x_obj["aqi_uba"] = aqi_uba;

        doc["timestamp"] = millis();
        serializeJson(doc, Serial);
        Serial.println();

        // Update all Zigbee endpoints
        if (Zigbee.connected()) {
            zbTempHum.setTemperature(temp_c);
            zbTempHum.setHumidity(humidity);
            zbTempHum.report();

            zbCO2.setCarbonDioxide((float)eco2);
            zbCO2.report();

            zbTVOC.setAnalogInput((float)etvoc);
            zbTVOC.reportAnalogInput();

            zbAQI.setAnalogInput((float)aqi);
            zbAQI.reportAnalogInput();
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// =====================================================================
//  Zigbee Pairing Helper
// =====================================================================

static bool is_zigbee_pairing() {
    if (!zigbee_pairing_active) return false;
    if (millis() - pairing_start_ms > PAIRING_TIMEOUT_MS) {
        zigbee_pairing_active = false;
        Serial.println("[zigbee] Pairing timed out");
        return false;
    }
    return true;
}

// =====================================================================
//  Startup Animation
// =====================================================================

static void startup_animation() {
    const unsigned long DURATION_MS = 3000;
    const unsigned long UPDATE_MS = 10;
    unsigned long start = millis();

    while (millis() - start < DURATION_MS) {
        float progress = (float)(millis() - start) / (float)DURATION_MS;
        uint16_t hue;
        if (progress <= 0.5f) {
            float ratio = progress * 2.0f;
            hue = HUE_GREEN - (uint16_t)(ratio * HUE_GREEN);
        } else {
            float ratio = (progress - 0.5f) * 2.0f;
            hue = (uint16_t)(ratio * HUE_GREEN);
        }

        uint32_t grb = get_color_from_hue(hue);
        uint8_t g = (grb >> 16) & 0xFF;
        uint8_t r = (grb >> 8) & 0xFF;
        uint8_t b = grb & 0xFF;
        r = (uint8_t)(r * 0.6f);
        g = (uint8_t)(g * 0.6f);
        b = (uint8_t)(b * 0.6f);

        for (int i = 0; i < NUM_CONTROLLED; i++) {
            strip.setPixelColor(i, strip.Color(r, g, b));
        }
        for (int i = NUM_CONTROLLED; i < NUM_LEDS; i++) {
            strip.setPixelColor(i, 0);
        }
        strip.show();
        delay(UPDATE_MS);
    }

    uint32_t grb = get_color_from_hue(HUE_GREEN);
    uint8_t g = (grb >> 16) & 0xFF;
    uint8_t r = (grb >> 8) & 0xFF;
    uint8_t b = grb & 0xFF;
    r = (uint8_t)(r * 0.6f);
    g = (uint8_t)(g * 0.6f);
    b = (uint8_t)(b * 0.6f);
    for (int i = 0; i < NUM_CONTROLLED; i++) {
        strip.setPixelColor(i, strip.Color(r, g, b));
    }
    strip.show();
}

// =====================================================================
//  setup()
// =====================================================================

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("TinySquare AirCube");

    // ── 1. NeoPixel + Animation ──
    strip.begin();
    strip.show();

    preferences.begin("aircube", true);
    bool is_pairing_boot = preferences.getBool("zb_pairing", false);
    preferences.end();

    if (!is_pairing_boot) {
        startup_animation();
        Serial.println("[led] Startup animation complete");
    } else {
        Serial.println("[led] Skipping animation (pairing mode)");
    }

    // ── 2. Zigbee endpoints (configure BEFORE begin) ──
    zbTempHum.setManufacturerAndModel("TinySquare", "AirCube");
    zbTempHum.addHumiditySensor(0, 100, 1);
    Zigbee.addEndpoint(&zbTempHum);

    zbCO2.setManufacturerAndModel("TinySquare", "AirCube CO2");
    Zigbee.addEndpoint(&zbCO2);

    zbTVOC.addAnalogInput();
    zbTVOC.setAnalogInputDescription("eTVOC");
    zbTVOC.setAnalogInputApplication(ESP_ZB_ZCL_AI_COUNT_UNITLESS_OTHER);
    zbTVOC.setAnalogInputResolution(1);
    zbTVOC.setManufacturerAndModel("TinySquare", "AirCube TVOC");
    Zigbee.addEndpoint(&zbTVOC);

    zbAQI.addAnalogInput();
    zbAQI.setAnalogInputDescription("AQI");
    zbAQI.setAnalogInputApplication(ESP_ZB_ZCL_AI_COUNT_UNITLESS_OTHER);
    zbAQI.setAnalogInputResolution(1);
    zbAQI.setManufacturerAndModel("TinySquare", "AirCube AQI");
    Zigbee.addEndpoint(&zbAQI);

    if (!Zigbee.begin()) {
        Serial.println("[zigbee] Failed to start! Rebooting...");
        delay(3000);
        ESP.restart();
    }
    Serial.println("[zigbee] Stack started");

    // Reporting config AFTER begin()
    zbTempHum.setReporting(1, 60, 0.5f);
    zbTempHum.setHumidityReporting(1, 60, 1.0f);
    zbCO2.setReporting(1, 60, 50.0f);
    zbTVOC.setAnalogInputReporting(1, 60, 10.0f);
    zbAQI.setAnalogInputReporting(1, 60, 5.0f);

    // Check pairing flag
    preferences.begin("aircube", false);
    bool pairing_requested = preferences.getBool("zb_pairing", false);
    if (pairing_requested) {
        preferences.putBool("zb_pairing", false);
        zigbee_pairing_active = true;
        pairing_start_ms = millis();
        Serial.println("[zigbee] Pairing mode active (from button press)");
    }
    preferences.end();

    // Non-blocking wait for Zigbee connection
    unsigned long zb_start = millis();
    unsigned long zb_timeout = zigbee_pairing_active ? PAIRING_TIMEOUT_MS : 10000;
    while (!Zigbee.connected() && (millis() - zb_start < zb_timeout)) {
        if (zigbee_pairing_active) {
            bool on = ((millis() / 250) % 2) == 0;
            uint8_t blue_val = on ? (uint8_t)(255 * 0.6f) : 0;
            for (int i = 0; i < NUM_CONTROLLED; i++) {
                strip.setPixelColor(i, strip.Color(0, 0, blue_val));
            }
            for (int i = NUM_CONTROLLED; i < NUM_LEDS; i++) {
                strip.setPixelColor(i, 0);
            }
            strip.show();
        }
        Serial.print(".");
        delay(250);
    }
    if (Zigbee.connected()) {
        zigbee_pairing_active = false;
        Serial.println("\n[zigbee] Connected!");
    } else if (zigbee_pairing_active) {
        zigbee_pairing_active = false;
        Serial.println("\n[zigbee] Pairing timed out");
    } else {
        Serial.println("\n[zigbee] No network yet (will keep trying)");
    }

    // ── 3. LED task ──
    led_mutex = xSemaphoreCreateMutex();
    xTaskCreate(led_task, "led_task", 4096, NULL, 10, NULL);
    led_set_color(get_color_from_hue(HUE_GREEN));

    // ── 4. Brightness ──
    preferences.begin("aircube", true);
    current_brightness_index = preferences.getInt("led_bright", 3);
    preferences.end();
    if (current_brightness_index < 0 || current_brightness_index >= num_brightness_levels) {
        current_brightness_index = 3;
    }
    led_set_intensity(brightness_levels[current_brightness_index]);

    // ── 5. I2C + Sensors ──
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);

    pEns210 = new ENS210();
    pEns210->begin();
    if (pEns210->init()) {
        Serial.println("[ens210] Initialized");
    } else {
        Serial.println("[ens210] Init failed!");
    }

    pEns16x = new ENS161();
    pEns16x->begin(&Wire, ENS16X_I2C_ADDR);
    if (pEns16x->init()) {
        pEns16x->startStandardMeasure();
        Serial.println("[ens16x] Initialized (Standard mode)");
    } else {
        Serial.println("[ens16x] Init failed!");
    }

    // ── 6. Button ──
    pinMode(PIN_BUTTON, INPUT_PULLDOWN);
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    attachInterruptArg(PIN_BUTTON, button_isr_handler, (void *)(uint32_t)PIN_BUTTON, RISING);
    xTaskCreate(button_task, "button_task", 4096, NULL, 5, NULL);
    Serial.println("[button] Initialized");

    // ── 7. Sensor Task ──
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);

    Serial.println("AirCube ready.");
}

// =====================================================================
//  loop()
// =====================================================================

void loop() {
    delay(20);

    if (is_zigbee_pairing()) {
        bool on = ((millis() / 250) % 2) == 0;
        led_set_color(on ? LED_COLOR_BLUE : LED_COLOR_OFF);
        return;
    }

    if (zigbee_pairing_active && Zigbee.connected()) {
        zigbee_pairing_active = false;
        Serial.println("[zigbee] Connected to network!");
    }

    int aqi = current_aqi;
    target_hue = (aqi >= AQI_MAX) ? 0 : aqi_to_hue(aqi);

    float hue_diff = (float)target_hue - current_hue;
    current_hue += hue_diff * TRANSITION_SPEED;

    led_set_color(get_color_from_hue((uint16_t)current_hue));
}
