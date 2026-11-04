/*
  =================================================================
   CAFE IOT SYSTEM - FRIDGE NODE (ESP32-S3)   + DHT22 inside fridge
  =================================================================
   - Door OPEN/CLOSED via a magnetic reed switch (40 ms debounce).
   - Door open for 5 s -> spoken warning, repeating every 5 s.
   - AC supply presence (ON / OFF / UNKNOWN) from an ISOLATED module.
   - NEW: DHT22 inside the fridge -> temperature + humidity.
       * Read in its own task every DHT_READ_INTERVAL_MS.
       * Invalid readings (NaN / out of range) are never published
         as values. After DHT_FAIL_LIMIT consecutive failures the
         sensor is declared FAULT and temp/humidity become UNKNOWN.
       * Optional over-temperature alert: temperature must stay above
         TEMP_HIGH_C for TEMP_HIGH_DELAY_MS (so a door opening does
         not trigger it) and must drop TEMP_HYSTERESIS_C below the
         limit to clear.
   - Publishes to MQTT (Last Will, retained state, heartbeat).

   WHAT THIS DOES NOT DO:
   - It does not prove the compressor is running; only that mains
     voltage is present where the detector is connected.
   - If this ESP32 loses power or Wi-Fi it cannot report anything;
     the dashboard must show UNKNOWN (never OFF). Power the ESP32
     from a SEPARATE supply from the one being monitored.

   !! MAINS SAFETY !!
   Never connect mains to any ESP32 pin/breadboard/USB. Use a
   certified isolated AC-presence module and a QUALIFIED ELECTRICIAN
   for all mains-side work.

   REQUIREMENTS
   - Board: "ESP32S3 Dev Module", ESP32 Arduino core 3.x
   - Libraries: PubSubClient, ArduinoJson (v6),
                "DHT sensor library" (Adafruit) and
                "Adafruit Unified Sensor"
   - voice_clip.h in this folder.

   WIRING
   - Reed switch:   GPIO4  <-> GND (INPUT_PULLUP)
   - AC detector:   OUT -> GPIO5, GND -> ESP32 GND (3.3 V-safe output)
   - MAX98357A:     BCLK -> GPIO15, LRC -> GPIO16, DIN -> GPIO17
   - DHT22:         VCC -> 3V3, GND -> GND, DATA -> GPIO6
                    A bare 4-pin DHT22 needs a 4.7k-10k pull-up
                    between DATA and VCC (3-pin breakout boards
                    already have one). Keep the wire short, and place
                    the sensor away from the evaporator fins / door
                    gasket and not touching the walls, for a
                    representative air temperature.
  =================================================================
*/

#if __has_include("esp_arduino_version.h")
#include "esp_arduino_version.h"
#endif
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR < 3)
#error "fridge_node.ino needs the ESP32 Arduino core 3.x (esp32 by Espressif, version 3.0.0 or newer)."
#endif

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include "driver/i2s_std.h"
#include "voice_clip.h"   // VOICE_SAMPLE_RATE, voiceClip[], VOICE_CLIP_SAMPLES

// ====================== CONFIGURE THESE ======================

const char* WIFI_SSID     = "BathanX_2.4";
const char* WIFI_PASSWORD = "BathanX4444";
const char* MQTT_HOST = "192.168.1.77";
const int   MQTT_PORT = 1883;
const char* MQTT_USER = "";
const char* MQTT_PASS = "";
const char* DEVICE_ID = "fridge01";

// ---- Pins ----
#define REED_PIN          4
#define POWER_DETECT_PIN  5
#define DHT_PIN           6
#define I2S_BCLK_PIN      15
#define I2S_LRC_PIN       16
#define I2S_DOUT_PIN      17

// ---- Reed switch polarity ----
const bool REED_LOW_MEANS_DOOR_CLOSED = true;

// ---- AC-presence detector output ----
const int  POWER_ACTIVE_LEVEL = HIGH;
const bool POWER_USE_PULLUP = false;
const bool POWER_SIGNAL_PULSED = false;
const uint32_t POWER_PULSE_WINDOW_MS = 100;

// ---- DHT22 ----
#define DHT_TYPE DHT22
const uint32_t DHT_READ_INTERVAL_MS = 5000;   // DHT22 needs >= 2 s between reads
const uint8_t  DHT_FAIL_LIMIT       = 3;      // consecutive bad reads -> sensor FAULT
const float    TEMP_CALIBRATION_C   = 0.0f;   // add/subtract to match a reference thermometer
const float    HUM_CALIBRATION_PCT  = 0.0f;

// ---- Over-temperature alert ----
const bool     TEMP_ALERT_ENABLED   = true;
const float    TEMP_HIGH_C          = 8.0f;              // typical fridge limit ~ 5 C; alert above this
const float    TEMP_HYSTERESIS_C    = 1.0f;              // must fall to TEMP_HIGH_C - this to clear
const uint32_t TEMP_HIGH_DELAY_MS   = 10UL * 60UL * 1000UL;  // must stay high this long

// ---- Timing ----
const uint32_t DOOR_DEBOUNCE_MS     = 40;
const uint32_t DOOR_WARN_DELAY_MS   = 5000;
const uint32_t DOOR_WARN_REPEAT_MS  = 5000;
const uint32_t POWER_VALIDATE_MS    = 1500;
const uint32_t HEARTBEAT_IDLE_MS    = 10000;
const uint32_t HEARTBEAT_OPEN_MS    = 5000;

const int VOLUME_PERCENT = 70;
// =============================================================

// ---- MQTT topics ----
const char* T_DOOR       = "cafe/fridge/door";          // OPEN | CLOSED | UNKNOWN   (retained)
const char* T_POWER      = "cafe/fridge/power";         // ON | OFF | UNKNOWN        (retained)
const char* T_TEMP       = "cafe/fridge/temperature";   // e.g. 4.3 | UNKNOWN        (retained)
const char* T_HUM        = "cafe/fridge/humidity";      // e.g. 61.2 | UNKNOWN       (retained)
const char* T_SENSOR     = "cafe/fridge/sensor";        // OK | FAULT                (retained)
const char* T_AVAIL      = "cafe/fridge/availability";  // online | offline (LWT)    (retained)
const char* T_TELEMETRY  = "cafe/fridge/telemetry";     // combined JSON, heartbeat
const char* T_ALERT      = "cafe/fridge/alert";         // event strings, not retained

// ---- Shared state ----
enum : uint8_t { DOOR_UNKNOWN = 0, DOOR_CLOSED = 1, DOOR_OPEN = 2 };
enum : uint8_t { PWR_UNKNOWN = 0, PWR_ON = 1, PWR_OFF = 2 };
enum : uint8_t {
  ALERT_DOOR_OPEN_WARNING = 0, ALERT_SUPPLY_LOST = 1, ALERT_SUPPLY_RESTORED = 2,
  ALERT_TEMP_HIGH = 3, ALERT_TEMP_NORMAL = 4, ALERT_SENSOR_FAULT = 5, ALERT_SENSOR_OK = 6
};

volatile uint8_t  g_door = DOOR_UNKNOWN;
volatile uint8_t  g_power = PWR_UNKNOWN;
volatile uint32_t g_doorOpenSince = 0;
volatile bool     g_doorDirty = false;
volatile bool     g_powerDirty = false;
volatile uint32_t g_lastPowerEdgeMs = 0;
QueueHandle_t     g_alertQueue = nullptr;

// DHT22 state (sensor task writes, main loop reads)
volatile float g_temp = 0;
volatile float g_hum = 0;
volatile bool  g_envValid = false;    // true only while the latest readings are good
volatile bool  g_sensorOk = true;     // false after DHT_FAIL_LIMIT consecutive failures
volatile bool  g_envDirty = false;    // new sample or sensor status change -> publish
DHT dht(DHT_PIN, DHT_TYPE);

// ---- Audio state ----
volatile bool g_playReq = false;
volatile bool g_abort = false;
volatile bool g_playing = false;
bool g_audioOk = false;
i2s_chan_handle_t g_tx = nullptr;
static const size_t AUDIO_CHUNK_FRAMES = 256;

// ---- Networking ----
WiFiClient netClient;
PubSubClient mqtt(netClient);
uint32_t lastWifiAttempt = 0;
uint32_t lastMqttAttempt = 0;
uint32_t lastTelemetry = 0;
bool forceTelemetry = true;
bool wasWifiUp = false;

const char* doorStr(uint8_t d) {
  return d == DOOR_OPEN ? "OPEN" : (d == DOOR_CLOSED ? "CLOSED" : "UNKNOWN");
}
const char* powerStr(uint8_t p) {
  return p == PWR_ON ? "ON" : (p == PWR_OFF ? "OFF" : "UNKNOWN");
}
const char* alertStr(uint8_t a) {
  switch (a) {
    case ALERT_DOOR_OPEN_WARNING: return "DOOR_OPEN_WARNING";
    case ALERT_SUPPLY_LOST:       return "SUPPLY_LOST";
    case ALERT_SUPPLY_RESTORED:   return "SUPPLY_RESTORED";
    case ALERT_TEMP_HIGH:         return "TEMP_HIGH";
    case ALERT_TEMP_NORMAL:       return "TEMP_NORMAL";
    case ALERT_SENSOR_FAULT:      return "SENSOR_FAULT";
    case ALERT_SENSOR_OK:         return "SENSOR_OK";
  }
  return "UNKNOWN_ALERT";
}

// =====================================================================
//  AUDIO (I2S -> MAX98357A)
// =====================================================================
bool audioBegin() {
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  chanCfg.auto_clear = true;
  if (i2s_new_channel(&chanCfg, &g_tx, NULL) != ESP_OK) return false;

  i2s_std_config_t stdCfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)I2S_BCLK_PIN,
      .ws   = (gpio_num_t)I2S_LRC_PIN,
      .dout = (gpio_num_t)I2S_DOUT_PIN,
      .din  = I2S_GPIO_UNUSED,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  if (i2s_channel_init_std_mode(g_tx, &stdCfg) != ESP_OK) return false;
  return i2s_channel_enable(g_tx) == ESP_OK;
}

void audioTask(void*) {
  static int16_t stereo[AUDIO_CHUNK_FRAMES * 2];
  for (;;) {
    if (!g_playReq) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    g_playReq = false;
    g_playing = true;

    size_t pos = 0;
    while (pos < VOICE_CLIP_SAMPLES && !g_abort) {
      size_t n = VOICE_CLIP_SAMPLES - pos;
      if (n > AUDIO_CHUNK_FRAMES) n = AUDIO_CHUNK_FRAMES;
      for (size_t i = 0; i < n; i++) {
        int16_t s = (int16_t)(((int32_t)voiceClip[pos + i] * VOLUME_PERCENT) / 100);
        stereo[2 * i] = s;
        stereo[2 * i + 1] = s;
      }
      size_t written = 0;
      i2s_channel_write(g_tx, stereo, n * 4, &written, 1000);
      pos += n;
    }
    g_playing = false;
  }
}

void requestPlay() {
  if (!g_audioOk || g_playing || g_playReq) return;
  g_abort = false;
  g_playReq = true;
}
void abortPlay() {
  g_playReq = false;
  g_abort = true;
}

// =====================================================================
//  LOCAL CONTROL TASK: door + supply. Never touches the network.
// =====================================================================
void IRAM_ATTR powerEdgeISR() { g_lastPowerEdgeMs = millis(); }

void queueAlert(uint8_t a) { xQueueSend(g_alertQueue, &a, 0); }

bool readPowerRaw(uint32_t now) {
  if (POWER_SIGNAL_PULSED) return (uint32_t)(now - g_lastPowerEdgeMs) < POWER_PULSE_WINDOW_MS;
  return digitalRead(POWER_DETECT_PIN) == POWER_ACTIVE_LEVEL;
}

void controlTask(void*) {
  int doorLast = digitalRead(REED_PIN);
  uint32_t doorChangeAt = millis();
  int doorStable = -1;
  uint32_t nextWarnAt = 0;
  bool alertSentThisOpen = false;
  bool pwrCandidate = readPowerRaw(millis());
  uint32_t pwrChangeAt = millis();

  for (;;) {
    uint32_t now = millis();

    // ---------- Door ----------
    int r = digitalRead(REED_PIN);
    if (r != doorLast) { doorLast = r; doorChangeAt = now; }
    if (doorLast != doorStable && (uint32_t)(now - doorChangeAt) >= DOOR_DEBOUNCE_MS) {
      doorStable = doorLast;
      bool closed = REED_LOW_MEANS_DOOR_CLOSED ? (doorStable == LOW) : (doorStable == HIGH);
      uint8_t newState = closed ? DOOR_CLOSED : DOOR_OPEN;
      if (newState != g_door) {
        g_door = newState;
        g_doorDirty = true;
        if (newState == DOOR_OPEN) {
          g_doorOpenSince = now;
          nextWarnAt = now + DOOR_WARN_DELAY_MS;
          alertSentThisOpen = false;
          Serial.println("[FRIDGE] Door OPEN");
        } else {
          g_doorOpenSince = 0;
          nextWarnAt = 0;
          abortPlay();
          Serial.println("[FRIDGE] Door CLOSED");
        }
      }
    }

    // ---------- Spoken warning schedule ----------
    if (g_door == DOOR_OPEN && nextWarnAt != 0 && (int32_t)(now - nextWarnAt) >= 0) {
      requestPlay();
      if (!alertSentThisOpen) {
        alertSentThisOpen = true;
        queueAlert(ALERT_DOOR_OPEN_WARNING);
      }
      nextWarnAt = now + DOOR_WARN_REPEAT_MS;
      Serial.println("[FRIDGE] Door open warning");
    }

    // ---------- Supply presence (validated) ----------
    bool rawPresent = readPowerRaw(now);
    if (rawPresent != pwrCandidate) { pwrCandidate = rawPresent; pwrChangeAt = now; }
    if ((uint32_t)(now - pwrChangeAt) >= POWER_VALIDATE_MS) {
      uint8_t newPwr = pwrCandidate ? PWR_ON : PWR_OFF;
      if (newPwr != g_power) {
        uint8_t prev = g_power;
        g_power = newPwr;
        g_powerDirty = true;
        Serial.printf("[FRIDGE] Supply %s\n", powerStr(newPwr));
        if (prev != PWR_UNKNOWN) {
          queueAlert(newPwr == PWR_OFF ? ALERT_SUPPLY_LOST : ALERT_SUPPLY_RESTORED);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// =====================================================================
//  SENSOR TASK: DHT22 reads (slow, ~5 ms bit-banged) kept off the
//  control task and the network loop.
// =====================================================================
void sensorTask(void*) {
  dht.begin();
  vTaskDelay(pdMS_TO_TICKS(2000));            // DHT22 needs ~2 s after power-up

  uint8_t  fails = 0;
  uint32_t overSince = 0;                     // millis() when temp first exceeded the limit (0 = not over)
  bool     highActive = false;

  for (;;) {
    float h = dht.readHumidity();
    float t = dht.readTemperature();          // Celsius
    bool good = !isnan(h) && !isnan(t) && t > -40.0f && t < 80.0f && h >= 0.0f && h <= 100.0f;
    uint32_t now = millis();

    if (good) {
      t += TEMP_CALIBRATION_C;
      h += HUM_CALIBRATION_PCT;
      if (h < 0) h = 0;
      if (h > 100) h = 100;
      fails = 0;
      g_temp = t;
      g_hum = h;
      g_envValid = true;
      g_envDirty = true;
      Serial.printf("[DHT] %.1f C  %.1f %%RH\n", t, h);

      if (!g_sensorOk) {
        g_sensorOk = true;
        queueAlert(ALERT_SENSOR_OK);
        Serial.println("[DHT] Sensor recovered");
      }

      if (TEMP_ALERT_ENABLED) {
        if (t > TEMP_HIGH_C) {
          if (overSince == 0) overSince = now;
          if (!highActive && (uint32_t)(now - overSince) >= TEMP_HIGH_DELAY_MS) {
            highActive = true;
            queueAlert(ALERT_TEMP_HIGH);
            Serial.println("[DHT] Over-temperature alert");
          }
        } else if (t <= TEMP_HIGH_C - TEMP_HYSTERESIS_C || !highActive) {
          overSince = 0;
          if (highActive) {
            highActive = false;
            queueAlert(ALERT_TEMP_NORMAL);
            Serial.println("[DHT] Temperature back to normal");
          }
        }
      }
    } else {
      if (fails < 255) fails++;
      Serial.printf("[DHT] Read failed (%u/%u)\n", fails, DHT_FAIL_LIMIT);
      if (fails >= DHT_FAIL_LIMIT && g_sensorOk) {
        g_sensorOk = false;
        g_envValid = false;                   // never publish stale numbers as if live
        g_envDirty = true;
        queueAlert(ALERT_SENSOR_FAULT);
        Serial.println("[DHT] Sensor FAULT");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(DHT_READ_INTERVAL_MS));
  }
}

// =====================================================================
//  NETWORKING (main loop)
// =====================================================================
void publishDoor()  { mqtt.publish(T_DOOR,  doorStr(g_door),   true); }
void publishPower() { mqtt.publish(T_POWER, powerStr(g_power), true); }

void publishEnv() {
  char buf[16];
  if (g_envValid) {
    snprintf(buf, sizeof(buf), "%.1f", (float)g_temp);
    mqtt.publish(T_TEMP, buf, true);
    snprintf(buf, sizeof(buf), "%.1f", (float)g_hum);
    mqtt.publish(T_HUM, buf, true);
  } else {
    mqtt.publish(T_TEMP, "UNKNOWN", true);
    mqtt.publish(T_HUM,  "UNKNOWN", true);
  }
  mqtt.publish(T_SENSOR, g_sensorOk ? "OK" : "FAULT", true);
}

void publishTelemetry() {
  uint32_t secs = 0;
  uint32_t since = g_doorOpenSince;
  if (g_door == DOOR_OPEN && since != 0) secs = (millis() - since) / 1000;

  StaticJsonDocument<320> doc;
  doc["door"] = doorStr(g_door);
  doc["power"] = powerStr(g_power);
  doc["door_open_seconds"] = secs;
  if (g_envValid) {
    doc["temp_c"] = serialized(String((float)g_temp, 1));
    doc["humidity"] = serialized(String((float)g_hum, 1));
  } else {
    doc["temp_c"] = nullptr;
    doc["humidity"] = nullptr;
  }
  doc["sensor"] = g_sensorOk ? "OK" : "FAULT";
  doc["rssi"] = WiFi.RSSI();
  doc["uptime_s"] = millis() / 1000;
  char buf[320];
  serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(T_TELEMETRY, buf, false);
}

void connectMqttOnce() {
  bool ok;
  if (strlen(MQTT_USER) > 0) {
    ok = mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS, T_AVAIL, 1, true, "offline");
  } else {
    ok = mqtt.connect(DEVICE_ID, T_AVAIL, 1, true, "offline");
  }
  if (ok) {
    Serial.println("[MQTT] Connected");
    mqtt.publish(T_AVAIL, "online", true);
    g_doorDirty = true;
    g_powerDirty = true;
    g_envDirty = true;
    forceTelemetry = true;
  } else {
    Serial.printf("[MQTT] Connect failed rc=%d, will retry\n", mqtt.state());
  }
}

void networkService() {
  uint32_t now = millis();
  bool wifiUp = (WiFi.status() == WL_CONNECTED);

  if (wifiUp != wasWifiUp) {
    wasWifiUp = wifiUp;
    if (wifiUp) { Serial.print("[WIFI] Connected, IP "); Serial.println(WiFi.localIP()); }
    else        { Serial.println("[WIFI] Lost - local door warnings keep working"); }
  }

  if (!wifiUp) {
    if (now - lastWifiAttempt > 20000) {
      lastWifiAttempt = now;
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    return;
  }

  if (!mqtt.connected()) {
    if (now - lastMqttAttempt > 5000) {
      lastMqttAttempt = now;
      connectMqttOnce();
    }
    return;
  }

  mqtt.loop();

  if (g_doorDirty)  { g_doorDirty = false;  publishDoor();  forceTelemetry = true; }
  if (g_powerDirty) { g_powerDirty = false; publishPower(); forceTelemetry = true; }
  if (g_envDirty)   { g_envDirty = false;   publishEnv(); }

  uint8_t a;
  while (xQueueReceive(g_alertQueue, &a, 0) == pdTRUE) {
    const char* msg = alertStr(a);
    mqtt.publish(T_ALERT, msg, false);
    Serial.printf("[MQTT] Alert: %s\n", msg);
  }

  uint32_t interval = (g_door == DOOR_OPEN) ? HEARTBEAT_OPEN_MS : HEARTBEAT_IDLE_MS;
  if (forceTelemetry || now - lastTelemetry >= interval) {
    forceTelemetry = false;
    lastTelemetry = now;
    publishTelemetry();
  }
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[FRIDGE] Booting fridge node");

  pinMode(REED_PIN, INPUT_PULLUP);
  pinMode(POWER_DETECT_PIN, POWER_USE_PULLUP ? INPUT_PULLUP : INPUT);
  if (POWER_SIGNAL_PULSED) {
    attachInterrupt(digitalPinToInterrupt(POWER_DETECT_PIN), powerEdgeISR, CHANGE);
  }

  g_alertQueue = xQueueCreate(12, sizeof(uint8_t));

  g_audioOk = audioBegin();
  if (g_audioOk) {
    xTaskCreatePinnedToCore(audioTask, "audio", 4096, NULL, 2, NULL, 0);
    Serial.println("[AUDIO] I2S ready");
  } else {
    Serial.println("[AUDIO] I2S init FAILED - warnings will be silent (alerts still sent)");
  }

  // Local logic starts before (and independently of) any networking.
  xTaskCreatePinnedToCore(controlTask, "control", 4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(sensorTask,  "dht",     4096, NULL, 1, NULL, 0);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(3);
}

void loop() {
  networkService();
  delay(5);
}
