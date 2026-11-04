/*
  =================================================================
   CAFE IOT SYSTEM - ENV NODE (ESP32 Dev Board)
  =================================================================
   ROLE:
   - Reads DHT22 (temperature + humidity) and an analog smoke
     sensor (e.g. MQ-2).
   - Publishes readings to cafe/env/data every READ_INTERVAL_MS.
   - Does NOT decide AC on/off itself - that logic lives on the
     HUB (and is mirrored/settable from the dashboard), so this
     node stays simple and swappable.

   LIBRARIES REQUIRED:
   - PubSubClient
   - ArduinoJson (v6)
   - DHT sensor library (by Adafruit) + Adafruit Unified Sensor

   BOARD: "ESP32 Dev Module"

   WIRING (defaults, change if needed):
   - DHT22 DATA -> GPIO4 (with a 10k pull-up resistor to 3.3V,
                   many DHT22 breakout modules already include this)
   - Smoke sensor (MQ-2 analog OUT) -> GPIO34 (ADC1, input-only pin)
  =================================================================
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// ============== CONFIGURE THESE ==============
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* MQTT_HOST = "192.168.1.100";
const int   MQTT_PORT = 1883;
const char* MQTT_USER = "";
const char* MQTT_PASS = "";
const char* DEVICE_ID = "env01";

#define DHTPIN   4
#define DHTTYPE  DHT22
#define SMOKE_PIN 34

const unsigned long READ_INTERVAL_MS = 5000; // publish every 5s
// ================================================

const char* T_ENV_STATUS = "cafe/env/status";
const char* T_ENV_DATA   = "cafe/env/data";

WiFiClient espClient;
PubSubClient mqtt(espClient);
DHT dht(DHTPIN, DHTTYPE);

unsigned long lastRead = 0;
unsigned long lastStatusPublish = 0;

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WIFI] Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println(" connected");
}

void connectMqtt() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  while (!mqtt.connected()) {
    Serial.print("[MQTT] Connecting...");
    bool ok;
    if (strlen(MQTT_USER) > 0) {
      ok = mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS, T_ENV_STATUS, 1, true, "offline");
    } else {
      ok = mqtt.connect(DEVICE_ID, T_ENV_STATUS, 1, true, "offline");
    }
    if (ok) {
      Serial.println("connected");
      mqtt.publish(T_ENV_STATUS, "online", true);
    } else {
      Serial.printf("failed rc=%d, retry in 2s\n", mqtt.state());
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  dht.begin();
  analogReadResolution(12); // 0-4095
  connectWiFi();
  connectMqtt();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqtt.connected()) connectMqtt();
  mqtt.loop();

  if (millis() - lastRead > READ_INTERVAL_MS) {
    lastRead = millis();

    float temp = dht.readTemperature();
    float hum  = dht.readHumidity();
    int   smoke = analogRead(SMOKE_PIN);

    if (isnan(temp) || isnan(hum)) {
      Serial.println("[ENV] DHT22 read failed - skipping this cycle");
      return;
    }

    StaticJsonDocument<128> doc;
    doc["temp"]  = temp;
    doc["hum"]   = hum;
    doc["smoke"] = smoke;
    doc["device"] = DEVICE_ID;

    char buf[128];
    size_t n = serializeJson(doc, buf);
    mqtt.publish(T_ENV_DATA, (const uint8_t*)buf, n, false);

    Serial.printf("[ENV] T=%.1fC H=%.1f%% Smoke=%d\n", temp, hum, smoke);
  }

  if (millis() - lastStatusPublish > 15000) {
    lastStatusPublish = millis();
    mqtt.publish(T_ENV_STATUS, "online", true);
  }
}
