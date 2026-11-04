/*
  =================================================================
   CAFE IOT SYSTEM - HUB FIRMWARE (ESP32-S3)
  =================================================================
   ROLE:
   - The "brain". Connects to WiFi + MQTT broker.
   - Subscribes to all node data/status topics.
   - Runs the temperature -> AC hysteresis control loop LOCALLY,
     so climate control keeps working even if the cloud/backend
     is temporarily unreachable.
   - Tracks which nodes are online (via status + heartbeat topics).
   - Tracks customer IN/OUT/INSIDE counters (mirrors backend, but
     lets the OLED show live counts without needing the dashboard).
   - Publishes a periodic aggregate JSON snapshot for the backend.
   - Drives a 0.9" OLED (SSD1306, 128x32, I2C) that cycles through
     status pages.

   LIBRARIES REQUIRED (Arduino Library Manager):
   - PubSubClient      by Nick O'Leary
   - ArduinoJson (v6)  by Benoit Blanchon
   - Adafruit GFX Library
   - Adafruit SSD1306

   BOARD: "ESP32S3 Dev Module" (Tools > Board)
   =================================================================
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

// ============== CONFIGURE THESE ==============
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// For local testing: mqtt://192.168.x.x  (use just the IP, no scheme, below)
// For cloud VPS later: change to your VPS IP / domain
const char* MQTT_HOST = "192.168.1.100";
const int   MQTT_PORT = 1883;
const char* MQTT_USER = "";      // leave blank if broker has no auth
const char* MQTT_PASS = "";

const char* DEVICE_ID = "hub01";

// 0.9" OLED = SSD1306 128x32. Change to 64 if you actually have a 0.96" 128x64.
#define OLED_WIDTH  128
#define OLED_HEIGHT 32
#define OLED_SDA    8      // ESP32-S3 devkits commonly use 8/9 - verify your board!
#define OLED_SCL    9
#define OLED_RESET  -1
#define OLED_ADDR   0x3C
// ================================================

// ---- MQTT TOPICS ----
const char* T_HUB_STATUS       = "cafe/hub/status";
const char* T_HUB_AGGREGATE    = "cafe/hub/aggregate";
const char* T_DOOR_STATUS      = "cafe/door/status";
const char* T_DOOR_EVENT       = "cafe/door/event";
const char* T_ENV_STATUS       = "cafe/env/status";
const char* T_ENV_DATA         = "cafe/env/data";
const char* T_CONTROL_STATUS   = "cafe/control/status";
const char* T_AC_STATE         = "cafe/control/ac/state";
const char* T_AC_SET           = "cafe/control/ac/set";
const char* T_CAMERA_STATUS    = "cafe/camera/status";
const char* T_CAMERA_CAPTURE   = "cafe/camera/capture";
const char* T_THRESHOLD_SET    = "cafe/config/threshold/set";
const char* T_FRIDGE_AVAIL     = "cafe/fridge/availability";
const char* T_FRIDGE_TELEMETRY = "cafe/fridge/telemetry";

WiFiClient espClient;
PubSubClient mqtt(espClient);
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
Preferences prefs;

// ---- Live state ----
float latestTemp = NAN, latestHum = NAN;
int   latestSmoke = -1;
bool  acState = false;               // false = OFF, true = ON
float onTemp = 24.0, offTemp = 18.0; // hysteresis thresholds (persisted)

unsigned long lastSeenDoor = 0, lastSeenEnv = 0, lastSeenControl = 0, lastSeenCamera = 0;
unsigned long lastSeenFridge = 0;
const unsigned long NODE_TIMEOUT_MS = 20000; // consider offline if no msg for 20s

// Last reported fridge state (from cafe/fridge/telemetry) - shown on the OLED
char fridgeDoor[8]  = "--";
char fridgePower[8] = "--";
unsigned long fridgeOpenSecs = 0;

unsigned long custIn = 0, custOut = 0;

unsigned long lastAggregatePublish = 0;
unsigned long lastOledSwitch = 0;
int oledPage = 0;

// ---------------- Helpers ----------------
bool isDoorOnline()    { return millis() - lastSeenDoor    < NODE_TIMEOUT_MS && lastSeenDoor    != 0; }
bool isEnvOnline()     { return millis() - lastSeenEnv     < NODE_TIMEOUT_MS && lastSeenEnv     != 0; }
bool isControlOnline() { return millis() - lastSeenControl < NODE_TIMEOUT_MS && lastSeenControl != 0; }
bool isCameraOnline()  { return millis() - lastSeenCamera  < NODE_TIMEOUT_MS && lastSeenCamera  != 0; }
bool isFridgeOnline()  { return millis() - lastSeenFridge  < NODE_TIMEOUT_MS && lastSeenFridge  != 0; }

// A node's retained Last Will message is the plain text "offline".
// Treat it as "gone now" instead of as proof of life.
void markSeen(unsigned long &lastSeen, const char* msg) {
  lastSeen = (strcmp(msg, "offline") == 0) ? 0 : millis();
}

void publishAcSet(bool on) {
  StaticJsonDocument<64> doc;
  doc["state"] = on ? "ON" : "OFF";
  char buf[64];
  serializeJson(doc, buf);
  mqtt.publish(T_AC_SET, buf, false);
  Serial.printf("[HUB] Publishing AC SET -> %s\n", on ? "ON" : "OFF");
}

void triggerCamera(const char* reason) {
  StaticJsonDocument<96> doc;
  doc["reason"] = reason;
  doc["event_id"] = String((unsigned long)millis()).c_str();
  char buf[96];
  serializeJson(doc, buf);
  mqtt.publish(T_CAMERA_CAPTURE, buf, false);
  Serial.printf("[HUB] Capture command sent (%s)\n", reason);
}

void saveThresholds() {
  prefs.putFloat("onTemp", onTemp);
  prefs.putFloat("offTemp", offTemp);
}

// ---------------- MQTT Callback ----------------
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  char msg[512];
  if (length >= sizeof(msg)) length = sizeof(msg) - 1;
  memcpy(msg, payload, length);
  msg[length] = '\0';
  String t = String(topic);

  Serial.printf("[MQTT IN] %s -> %s\n", topic, msg);

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, msg);

  if (t == T_DOOR_STATUS) {
    markSeen(lastSeenDoor, msg);
  } else if (t == T_DOOR_EVENT) {
    lastSeenDoor = millis();
    if (!err) {
      const char* ev = doc["event"];
      if (ev) {
        if (strcmp(ev, "IN") == 0) {
          custIn++;
          triggerCamera("IN");
        } else if (strcmp(ev, "OUT") == 0) {
          custOut++;
          triggerCamera("OUT");
        }
      }
    }
  } else if (t == T_ENV_STATUS) {
    markSeen(lastSeenEnv, msg);
  } else if (t == T_ENV_DATA) {
    lastSeenEnv = millis();
    if (!err) {
      if (doc.containsKey("temp"))  latestTemp  = doc["temp"];
      if (doc.containsKey("hum"))   latestHum   = doc["hum"];
      if (doc.containsKey("smoke")) latestSmoke = doc["smoke"];

      // ---- Local hysteresis control loop (works even if backend is down) ----
      if (!isnan(latestTemp)) {
        if (latestTemp >= onTemp && !acState) {
          publishAcSet(true);
        } else if (latestTemp <= offTemp && acState) {
          publishAcSet(false);
        }
      }
    }
  } else if (t == T_CONTROL_STATUS) {
    markSeen(lastSeenControl, msg);
  } else if (t == T_AC_STATE) {
    lastSeenControl = millis();
    if (!err) {
      const char* st = doc["state"];
      if (st) acState = (strcmp(st, "ON") == 0);
    }
  } else if (t == T_CAMERA_STATUS) {
    markSeen(lastSeenCamera, msg);
  } else if (t == T_FRIDGE_AVAIL) {
    markSeen(lastSeenFridge, msg);
    if (lastSeenFridge == 0) {            // controller offline -> its sensor states are unknown
      strcpy(fridgeDoor, "--");
      strcpy(fridgePower, "--");
      fridgeOpenSecs = 0;
    }
  } else if (t == T_FRIDGE_TELEMETRY) {
    lastSeenFridge = millis();
    if (!err) {
      const char* d = doc["door"];
      const char* p = doc["power"];
      if (d) { strncpy(fridgeDoor, d, sizeof(fridgeDoor) - 1);  fridgeDoor[sizeof(fridgeDoor) - 1] = '\0'; }
      if (p) { strncpy(fridgePower, p, sizeof(fridgePower) - 1); fridgePower[sizeof(fridgePower) - 1] = '\0'; }
      fridgeOpenSecs = doc["door_open_seconds"] | 0;
    }
  } else if (t == T_THRESHOLD_SET) {
    if (!err) {
      if (doc.containsKey("on_temp"))  onTemp  = doc["on_temp"];
      if (doc.containsKey("off_temp")) offTemp = doc["off_temp"];
      saveThresholds();
      Serial.printf("[HUB] Threshold updated: ON=%.1f OFF=%.1f\n", onTemp, offTemp);
    }
  }
}

// ---------------- WiFi / MQTT connect ----------------
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WIFI] Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("[WIFI] Connected. IP: ");
  Serial.println(WiFi.localIP());
}

void connectMqtt() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(768);   // aggregate JSON (~450 B) + MQTT header must fit

  while (!mqtt.connected()) {
    Serial.print("[MQTT] Connecting...");
    // Last Will: mark hub offline if it dies unexpectedly
    bool ok;
    if (strlen(MQTT_USER) > 0) {
      ok = mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS, T_HUB_STATUS, 1, true, "offline");
    } else {
      ok = mqtt.connect(DEVICE_ID, T_HUB_STATUS, 1, true, "offline");
    }
    if (ok) {
      Serial.println("connected");
      mqtt.publish(T_HUB_STATUS, "online", true);
      mqtt.subscribe(T_DOOR_STATUS);
      mqtt.subscribe(T_DOOR_EVENT);
      mqtt.subscribe(T_ENV_STATUS);
      mqtt.subscribe(T_ENV_DATA);
      mqtt.subscribe(T_CONTROL_STATUS);
      mqtt.subscribe(T_AC_STATE);
      mqtt.subscribe(T_CAMERA_STATUS);
      mqtt.subscribe(T_THRESHOLD_SET);
      mqtt.subscribe(T_FRIDGE_AVAIL);
      mqtt.subscribe(T_FRIDGE_TELEMETRY);
    } else {
      Serial.printf("failed, rc=%d. Retrying in 2s\n", mqtt.state());
      delay(2000);
    }
  }
}

// ---------------- OLED ----------------
void drawOled() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);

  switch (oledPage) {
    case 0: {
      display.println("SYSTEM STATUS");
      display.print("WiFi:"); display.print(WiFi.status() == WL_CONNECTED ? "OK " : "-- ");
      display.print("MQTT:"); display.println(mqtt.connected() ? "OK" : "--");
      display.print("D:"); display.print(isDoorOnline() ? "UP " : "DN ");
      display.print("E:"); display.print(isEnvOnline() ? "UP " : "DN ");
      display.print("C:"); display.print(isControlOnline() ? "UP " : "DN ");
      display.print("Cam:"); display.println(isCameraOnline() ? "UP" : "DN");
      break;
    }
    case 1: {
      display.println("ENVIRONMENT");
      display.print("Temp: "); display.print(isnan(latestTemp) ? -1 : latestTemp); display.println(" C");
      display.print("Hum : "); display.print(isnan(latestHum) ? -1 : latestHum); display.println(" %");
      display.print("Smoke: "); display.println(latestSmoke);
      break;
    }
    case 2: {
      display.println("AC / CLIMATE");
      display.print("State: "); display.println(acState ? "ON" : "OFF");
      display.print("On @ "); display.print(onTemp); display.println("C");
      display.print("Off@ "); display.print(offTemp); display.println("C");
      break;
    }
    case 3: {
      display.println("CUSTOMERS TODAY");
      display.print("IN : "); display.println(custIn);
      display.print("OUT: "); display.println(custOut);
      long inside = (long)custIn - (long)custOut;
      if (inside < 0) inside = 0;
      display.print("NOW: "); display.println(inside);
      break;
    }
    case 4: {
      display.println("FRIDGE");
      display.print("Ctrl: "); display.println(isFridgeOnline() ? "ONLINE" : "OFFLINE");
      display.print("Door: ");
      if (isFridgeOnline()) {
        display.print(fridgeDoor);
        if (strcmp(fridgeDoor, "OPEN") == 0) { display.print(" "); display.print(fridgeOpenSecs); display.print("s"); }
        display.println();
      } else {
        display.println("--");
      }
      display.print("Power: "); display.println(isFridgeOnline() ? fridgePower : "UNKNOWN");
      break;
    }
  }
  display.display();
}

// ---------------- Aggregate publish ----------------
void publishAggregate() {
  StaticJsonDocument<640> doc;
  doc["wifi"] = WiFi.status() == WL_CONNECTED;
  doc["mqtt"] = mqtt.connected();
  doc["door_online"] = isDoorOnline();
  doc["env_online"] = isEnvOnline();
  doc["control_online"] = isControlOnline();
  doc["camera_online"] = isCameraOnline();
  doc["fridge_online"] = isFridgeOnline();
  doc["fridge_door"] = isFridgeOnline() ? fridgeDoor : "--";
  doc["fridge_power"] = isFridgeOnline() ? fridgePower : "UNKNOWN";
  doc["temp"] = latestTemp;
  doc["hum"] = latestHum;
  doc["smoke"] = latestSmoke;
  doc["ac_state"] = acState ? "ON" : "OFF";
  doc["on_temp"] = onTemp;
  doc["off_temp"] = offTemp;
  doc["customers_in"] = custIn;
  doc["customers_out"] = custOut;
  doc["uptime_s"] = millis() / 1000;

  char buf[640];
  size_t n = serializeJson(doc, buf);
  mqtt.publish(T_HUB_AGGREGATE, (const uint8_t*)buf, n, true);
}

// ---------------- Setup / Loop ----------------
void setup() {
  Serial.begin(115200);
  delay(300);

  prefs.begin("hubcfg", false);
  onTemp  = prefs.getFloat("onTemp", 24.0);
  offTemp = prefs.getFloat("offTemp", 18.0);

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[OLED] SSD1306 init failed - check wiring/address");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("CAFE IOT HUB");
    display.println("Booting...");
    display.display();
  }

  connectWiFi();
  connectMqtt();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqtt.connected()) connectMqtt();
  mqtt.loop();

  if (millis() - lastAggregatePublish > 5000) {
    lastAggregatePublish = millis();
    publishAggregate();
  }

  if (millis() - lastOledSwitch > 3000) {
    lastOledSwitch = millis();
    oledPage = (oledPage + 1) % 5;
    drawOled();
  }
}
