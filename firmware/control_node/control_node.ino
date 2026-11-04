/*
  =================================================================
   CAFE IOT SYSTEM - CONTROL NODE (ESP32-S3, IR Transmitter version)
  =================================================================
   ROLE:
   - Drives an IR LED (via an Iduino/generic IR transmitter module)
     to remotely command the AC, using its native IR remote protocol
     (Goodweather, captured from your real remote) - instead of a
     relay wired into the mains.
   - Listens on cafe/control/ac/set for {"state":"ON"|"OFF"}
     commands (sent by the HUB, or manually from the dashboard).
   - Publishes the CURRENT (commanded) state on
     cafe/control/ac/state whenever it changes, and periodically,
     so the dashboard/hub know what was last sent.

   !! IMPORTANT LIMITATION !!
   IR is fire-and-forget: there is no feedback confirming the AC
   unit actually received/obeyed the signal (LOS, low IR range,
   someone using the physical remote independently, etc. can all
   desync "commanded" state from the AC's real state). The state
   this node reports is "what we last told the AC to do," not a
   verified reading. Point the IR LED with a clear, close line of
   sight to the AC's receiver window.

   LIBRARIES REQUIRED:
   - PubSubClient
   - ArduinoJson (v6)
   - IRremoteESP8266 (by crankyoldgit) - provides IRsend + ir_Goodweather.h

   BOARD: "ESP32S3 Dev Module" (Tools > Board)

   WIRING:
   - IR transmitter module "S"/signal pin -> GPIO5 (IR_LED_PIN below)
   - IR transmitter VCC -> 3.3V or 5V (check your module's spec)
   - IR transmitter GND -> GND
   - Aim the IR LED directly at the AC unit's receiver window,
     ideally within 1-3 meters with a clear line of sight.

   HOW TO GET YOUR AC's CODES (if you need to re-capture):
   - Wire an IR RECEIVER module (e.g. TSOP38238) to a spare ESP32,
     flash the IRremoteESP8266 "IRrecvDumpV3" example, point your
     real AC remote at it, press Power - the Serial Monitor prints
     the raw hex code and protocol name to paste in below.
  =================================================================
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <IRsend.h>
#include <ir_Goodweather.h>

// ============== CONFIGURE THESE ==============
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* MQTT_HOST = "192.168.1.100";
const int   MQTT_PORT = 1883;
const char* MQTT_USER = "";
const char* MQTT_PASS = "";
const char* DEVICE_ID = "control01";

#define IR_LED_PIN 5   // Iduino/generic IR transmitter "S" pin

// ---- YOUR CAPTURED CODES (replace with your own remote's dump) ----
const uint64_t IR_CODE_ON  = 0xD50606000000;  // captured: Power On
const uint64_t IR_CODE_OFF = 0xD50604000000;  // captured: Power Off
// (Temp Up/Down codes aren't needed for this project - only ON/OFF
//  is used by the temperature hysteresis logic on the Hub - but you
//  can capture and wire those in later the same way if you want
//  manual temp-step control from the dashboard too.)

// If true, forces the AC to a known OFF state once at boot (mirrors
// the old relay's "always start OFF for safety" behavior). Set to
// false if you don't want the node to touch the AC on every reboot.
const bool SEND_INITIAL_OFF = true;
// ================================================

const char* T_CONTROL_STATUS = "cafe/control/status";
const char* T_AC_STATE       = "cafe/control/ac/state";
const char* T_AC_SET         = "cafe/control/ac/set";

WiFiClient espClient;
PubSubClient mqtt(espClient);
IRsend irsend(IR_LED_PIN);

bool acState = false; // last COMMANDED state (see IR limitation note above)
unsigned long lastStatusPublish = 0;

void sendAcCommand(bool on) {
  if (on) {
    irsend.sendGoodweather(IR_CODE_ON);
    Serial.println("[CTRL] IR sent: AC ON");
  } else {
    irsend.sendGoodweather(IR_CODE_OFF);
    Serial.println("[CTRL] IR sent: AC OFF");
  }
  acState = on;
}

void publishAcState() {
  StaticJsonDocument<64> doc;
  doc["state"] = acState ? "ON" : "OFF";
  char buf[64];
  size_t n = serializeJson(doc, buf);
  mqtt.publish(T_AC_STATE, (const uint8_t*)buf, n, true); // retained so late subscribers see it immediately
  Serial.printf("[CTRL] AC state published: %s\n", acState ? "ON" : "OFF");
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  char msg[128];
  if (length >= sizeof(msg)) length = sizeof(msg) - 1;
  memcpy(msg, payload, length);
  msg[length] = '\0';

  if (String(topic) == T_AC_SET) {
    StaticJsonDocument<64> doc;
    if (deserializeJson(doc, msg) == DeserializationError::Ok) {
      const char* st = doc["state"];
      if (st) {
        bool wantOn = (strcmp(st, "ON") == 0);
        if (wantOn != acState) {
          sendAcCommand(wantOn);
          publishAcState();
        }
      }
    }
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
  mqtt.setCallback(onMqttMessage);
  while (!mqtt.connected()) {
    Serial.print("[MQTT] Connecting...");
    bool ok;
    if (strlen(MQTT_USER) > 0) {
      ok = mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS, T_CONTROL_STATUS, 1, true, "offline");
    } else {
      ok = mqtt.connect(DEVICE_ID, T_CONTROL_STATUS, 1, true, "offline");
    }
    if (ok) {
      Serial.println("connected");
      mqtt.publish(T_CONTROL_STATUS, "online", true);
      mqtt.subscribe(T_AC_SET);
      publishAcState(); // announce current (commanded) state on (re)connect
    } else {
      Serial.printf("failed rc=%d, retry in 2s\n", mqtt.state());
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  irsend.begin();

  if (SEND_INITIAL_OFF) {
    sendAcCommand(false); // start from a known state, same intent as the old relay's safety default
  }

  connectWiFi();
  connectMqtt();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqtt.connected()) connectMqtt();
  mqtt.loop();

  if (millis() - lastStatusPublish > 15000) {
    lastStatusPublish = millis();
    mqtt.publish(T_CONTROL_STATUS, "online", true);
    publishAcState();
  }
}
