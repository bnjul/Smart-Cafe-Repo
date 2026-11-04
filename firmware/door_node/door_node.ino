/*
  =================================================================
   CAFE IOT SYSTEM - DOOR NODE (ESP32 Dev Module)
  =================================================================
   ROLE:
   - One REED (magnetic) switch on the door frame.
   - One PIR motion sensor pointed across the doorway.
   - Detects direction of crossing using ORDER of triggers:
       REED first, then PIR within 10s  -> CUSTOMER IN
       PIR first, then REED within 10s  -> CUSTOMER OUT
   - If only one sensor fires and the other doesn't follow within
     10 seconds, the pending state is DROPPED and reset to idle
     (handles false triggers / someone just touching the door).
   - Publishes the event to cafe/door/event. The HUB will react by
     telling the camera node to capture photos.

   LIBRARIES REQUIRED:
   - PubSubClient
   - ArduinoJson (v6)

   BOARD: "ESP32 Dev Module"

   WIRING (defaults, change if needed):
   - REED switch: one leg -> GPIO27, other leg -> GND
                  (uses internal pull-up; switch closes to GND)
   - PIR sensor:  OUT -> GPIO26, VCC -> 5V, GND -> GND
                  (most PIR modules output HIGH on motion)
  =================================================================
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ============== CONFIGURE THESE ==============
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* MQTT_HOST = "192.168.1.100";
const int   MQTT_PORT = 1883;
const char* MQTT_USER = "";
const char* MQTT_PASS = "";
const char* DEVICE_ID = "door01";

#define REED_PIN 27   // digital, interrupt-capable
#define PIR_PIN  26   // digital, interrupt-capable

const unsigned long CROSS_TIMEOUT_MS = 10000; // 10 seconds as specified
const unsigned long DEBOUNCE_MS      = 200;   // ignore re-triggers within this window
// ================================================

const char* T_DOOR_STATUS = "cafe/door/status";
const char* T_DOOR_EVENT  = "cafe/door/event";

WiFiClient espClient;
PubSubClient mqtt(espClient);

volatile bool reedFlag = false;
volatile bool pirFlag  = false;
volatile unsigned long reedIsrTime = 0;
volatile unsigned long pirIsrTime  = 0;

// Debounced/confirmed trigger state
bool reedTriggered = false;
bool pirTriggered  = false;
unsigned long reedTriggeredAt = 0;
unsigned long pirTriggeredAt  = 0;

unsigned long lastStatusPublish = 0;

void IRAM_ATTR reedISR() {
  unsigned long now = millis();
  if (now - reedIsrTime > DEBOUNCE_MS) {
    reedIsrTime = now;
    reedFlag = true;
  }
}

void IRAM_ATTR pirISR() {
  unsigned long now = millis();
  if (now - pirIsrTime > DEBOUNCE_MS) {
    pirIsrTime = now;
    pirFlag = true;
  }
}

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
      ok = mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS, T_DOOR_STATUS, 1, true, "offline");
    } else {
      ok = mqtt.connect(DEVICE_ID, T_DOOR_STATUS, 1, true, "offline");
    }
    if (ok) {
      Serial.println("connected");
      mqtt.publish(T_DOOR_STATUS, "online", true);
    } else {
      Serial.printf("failed rc=%d, retry in 2s\n", mqtt.state());
      delay(2000);
    }
  }
}

void publishEvent(const char* eventType) {
  StaticJsonDocument<128> doc;
  doc["event"] = eventType;
  doc["device"] = DEVICE_ID;
  doc["millis"] = millis();
  char buf[128];
  size_t n = serializeJson(doc, buf);
  mqtt.publish(T_DOOR_EVENT, (const uint8_t*)buf, n, false);
  Serial.printf("[DOOR] Event published: %s\n", eventType);
}

void resetState() {
  reedTriggered = false;
  pirTriggered = false;
}

void setup() {
  Serial.begin(115200);
  pinMode(REED_PIN, INPUT_PULLUP);
  pinMode(PIR_PIN, INPUT);

  attachInterrupt(digitalPinToInterrupt(REED_PIN), reedISR, FALLING); // reed closes to GND
  attachInterrupt(digitalPinToInterrupt(PIR_PIN), pirISR, RISING);    // PIR goes HIGH on motion

  connectWiFi();
  connectMqtt();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqtt.connected()) connectMqtt();
  mqtt.loop();

  // Latch confirmed triggers from ISR flags
  if (reedFlag) {
    noInterrupts();
    reedFlag = false;
    interrupts();
    if (!reedTriggered) {
      reedTriggered = true;
      reedTriggeredAt = millis();
      Serial.println("[DOOR] REED triggered");
    }
  }
  if (pirFlag) {
    noInterrupts();
    pirFlag = false;
    interrupts();
    if (!pirTriggered) {
      pirTriggered = true;
      pirTriggeredAt = millis();
      Serial.println("[DOOR] PIR triggered");
    }
  }

  // Both triggered -> decide direction by which came FIRST
  if (reedTriggered && pirTriggered) {
    if (reedTriggeredAt <= pirTriggeredAt) {
      publishEvent("IN");   // REED first, then PIR -> customer walking IN
    } else {
      publishEvent("OUT");  // PIR first, then REED -> customer walking OUT
    }
    resetState();
  }

  // Timeout: only one sensor fired, other never followed within 10s -> drop it
  unsigned long now = millis();
  if (reedTriggered && !pirTriggered && (now - reedTriggeredAt > CROSS_TIMEOUT_MS)) {
    Serial.println("[DOOR] REED timeout - dropping (no PIR follow-up)");
    resetState();
  }
  if (pirTriggered && !reedTriggered && (now - pirTriggeredAt > CROSS_TIMEOUT_MS)) {
    Serial.println("[DOOR] PIR timeout - dropping (no REED follow-up)");
    resetState();
  }

  if (millis() - lastStatusPublish > 15000) {
    lastStatusPublish = millis();
    mqtt.publish(T_DOOR_STATUS, "online", true);
  }
}
