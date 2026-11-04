/*
  =================================================================
   CAFE IOT SYSTEM - CAMERA NODE (AI-Thinker ESP32-CAM)
  =================================================================
   ROLE:
   - Subscribes to cafe/camera/capture (JSON: {"reason":"IN"/"OUT"}).
   - On receipt, captures CAPTURE_COUNT photos, CAPTURE_INTERVAL_MS
     apart, and uploads each directly to the backend over
     HTTP(S) POST multipart/form-data - NOT over MQTT (MQTT is not
     suited for large binary payloads).
   - Works over plain HTTP while running locally, and over HTTPS
     once you move the backend to a cloud VPS with a domain
     (just flip USE_HTTPS to true and set SERVER_HOST to your
     domain).

   LIBRARIES REQUIRED:
   - PubSubClient
   - ArduinoJson (v6)
   - (esp_camera.h / WiFiClientSecure / HTTPClient are built into
     the ESP32 board package - no separate install needed)

   BOARD: "AI Thinker ESP32-CAM" (Tools > Board)
   PARTITION SCHEME: "Huge APP (3MB No OTA/1MB SPIFFS)" recommended

   FLASHING NOTE: ESP32-CAM has no onboard USB. Use an FTDI/USB-TTL
   adapter, connect GPIO0 to GND ONLY while flashing, remove after,
   then press the RESET button to run normally. See TROUBLESHOOTING.md.

   POWER NOTE: Powering the camera from a USB-serial adapter's 3.3V
   pin usually causes brownout resets during capture/WiFi TX. Use a
   solid external 5V >=1A supply into the 5V pin.
  =================================================================
*/

#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ============== CONFIGURE THESE ==============
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const char* MQTT_HOST = "192.168.1.100";
const int   MQTT_PORT = 1883;
const char* MQTT_USER = "";
const char* MQTT_PASS = "";
const char* DEVICE_ID = "camera01";

// ---- Backend upload endpoint ----
bool USE_HTTPS       = false;                 // true once deployed to cloud VPS w/ domain
const char* SERVER_HOST = "192.168.1.100";    // change to your domain when USE_HTTPS = true
const int   SERVER_PORT = 3000;               // 443 typical for HTTPS behind nginx
const char* UPLOAD_PATH = "/api/photos/upload";
const char* UPLOAD_SECRET = "changeme123";    // MUST match backend .env CAMERA_UPLOAD_SECRET

const int CAPTURE_COUNT = 3;
const unsigned long CAPTURE_INTERVAL_MS = 1500;
// ================================================

// ---- AI-Thinker ESP32-CAM pin map (standard, do not change unless
//      you have a different camera board) ----
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22
#define FLASH_LED_PIN      4

const char* T_CAMERA_STATUS  = "cafe/camera/status";
const char* T_CAMERA_CAPTURE = "cafe/camera/capture";
const char* T_UPLOAD_DONE    = "cafe/camera/upload/done";

WiFiClient plainClient;
WiFiClientSecure secureClient;
PubSubClient mqtt(plainClient); // MQTT always plain (broker on LAN/VPS port 1883)

unsigned long lastStatusPublish = 0;
volatile bool captureRequested = false;
char pendingReason[16] = "IN";
char pendingEventId[32] = "";

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  // Field was renamed sscb -> sccb in ESP32 Arduino core 3.x (old name still works but is deprecated)
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
#else
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
#endif
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_UXGA; // up to 1600x1200
    config.jpeg_quality = 12;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 15;
    config.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] Camera init failed: 0x%x\n", err);
    return false;
  }
  return true;
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

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  char msg[160];
  if (length >= sizeof(msg)) length = sizeof(msg) - 1;
  memcpy(msg, payload, length);
  msg[length] = '\0';

  if (String(topic) == T_CAMERA_CAPTURE) {
    StaticJsonDocument<160> doc;
    if (deserializeJson(doc, msg) == DeserializationError::Ok) {
      const char* reason = doc["reason"] | "IN";
      const char* evId   = doc["event_id"] | "";
      strncpy(pendingReason, reason, sizeof(pendingReason) - 1);
      strncpy(pendingEventId, evId, sizeof(pendingEventId) - 1);
      captureRequested = true;
      Serial.printf("[CAM] Capture requested, reason=%s\n", pendingReason);
    }
  }
}

void connectMqtt() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  while (!mqtt.connected()) {
    Serial.print("[MQTT] Connecting...");
    bool ok;
    if (strlen(MQTT_USER) > 0) {
      ok = mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS, T_CAMERA_STATUS, 1, true, "offline");
    } else {
      ok = mqtt.connect(DEVICE_ID, T_CAMERA_STATUS, 1, true, "offline");
    }
    if (ok) {
      Serial.println("connected");
      mqtt.publish(T_CAMERA_STATUS, "online", true);
      mqtt.subscribe(T_CAMERA_CAPTURE);
    } else {
      Serial.printf("failed rc=%d, retry in 2s\n", mqtt.state());
      delay(2000);
    }
  }
}

// Uploads one JPEG frame via multipart/form-data POST.
bool uploadPhoto(camera_fb_t* fb, const char* reason, const char* eventId, int index) {
  WiFiClient* client;
  int port = SERVER_PORT;

  if (USE_HTTPS) {
    secureClient.setInsecure(); // simplest option; for production, pin the server cert/CA instead
    client = &secureClient;
  } else {
    client = &plainClient;
  }

  if (!client->connect(SERVER_HOST, port)) {
    Serial.println("[UPLOAD] Connection to server failed");
    return false;
  }

  String boundary = "----CafeCamBoundary7MA4YWx";
  String head = "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"device\"\r\n\r\n" + String(DEVICE_ID) + "\r\n" +
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"reason\"\r\n\r\n" + String(reason) + "\r\n" +
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"event_id\"\r\n\r\n" + String(eventId) + "\r\n" +
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"index\"\r\n\r\n" + String(index) + "\r\n" +
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"photo\"; filename=\"" + String(eventId) + "_" + String(index) + ".jpg\"\r\n"
    "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  size_t totalLen = head.length() + fb->len + tail.length();

  client->printf("POST %s HTTP/1.1\r\n", UPLOAD_PATH);
  client->printf("Host: %s\r\n", SERVER_HOST);
  client->println("Connection: close");
  client->printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary.c_str());
  client->printf("Content-Length: %u\r\n", (unsigned)totalLen);
  client->printf("x-upload-secret: %s\r\n", UPLOAD_SECRET);
  client->println();
  client->print(head);
  client->write(fb->buf, fb->len);
  client->print(tail);

  // Wait briefly for a response, then close (fire-and-forget style is fine here)
  unsigned long start = millis();
  while (client->connected() && millis() - start < 5000) {
    if (client->available()) {
      String statusLine = client->readStringUntil('\n');
      Serial.print("[UPLOAD] Response: ");
      Serial.println(statusLine);
      break;
    }
    delay(10);
  }
  client->stop();
  return true;
}

void doCaptureSequence(const char* reason, const char* eventId) {
  for (int i = 0; i < CAPTURE_COUNT; i++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("[CAM] Frame capture failed");
      continue;
    }
    Serial.printf("[CAM] Captured photo %d/%d, %u bytes\n", i + 1, CAPTURE_COUNT, fb->len);
    uploadPhoto(fb, reason, eventId, i + 1);
    esp_camera_fb_return(fb);

    if (i < CAPTURE_COUNT - 1) delay(CAPTURE_INTERVAL_MS);
  }

  StaticJsonDocument<96> doc;
  doc["event_id"] = eventId;
  doc["count"] = CAPTURE_COUNT;
  char buf[96];
  size_t n = serializeJson(doc, buf);
  mqtt.publish(T_UPLOAD_DONE, (const uint8_t*)buf, n, false);
}

void setup() {
  Serial.begin(115200);
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  if (!initCamera()) {
    Serial.println("[CAM] FATAL: camera init failed, halting");
    while (true) delay(1000);
  }

  connectWiFi();
  connectMqtt();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqtt.connected()) connectMqtt();
  mqtt.loop();

  if (captureRequested) {
    captureRequested = false;
    String evId = strlen(pendingEventId) > 0 ? String(pendingEventId) : String(millis());
    doCaptureSequence(pendingReason, evId.c_str());
  }

  if (millis() - lastStatusPublish > 15000) {
    lastStatusPublish = millis();
    mqtt.publish(T_CAMERA_STATUS, "online", true);
  }
}
