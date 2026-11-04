/*
  =========================================================
   GET MAC ADDRESS - Utility Sketch
  =========================================================
  Flash this to ANY ESP32 board (S3, Dev Module, ESP32-CAM,
  etc.) FIRST, before flashing the real firmware.

  It prints the board's WiFi Station MAC address to the
  Serial Monitor every 5 seconds. Use this to:
    - Label your physical boards (write MAC on masking tape!)
    - Whitelist devices on your router (DHCP reservation)
    - Keep a record in your project notes

  For ESP32-CAM (AI-Thinker) boards without an onboard
  USB-serial chip, wire an FTDI/USB-TTL adapter:
      FTDI TX  -> ESP32-CAM U0R (GPIO3)
      FTDI RX  -> ESP32-CAM U0T (GPIO1)
      FTDI GND -> GND
      FTDI 5V  -> 5V
      GPIO0    -> GND (only while flashing, remove after)

  Open Serial Monitor at 115200 baud.
  =========================================================
*/

#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_STA);   // Must set STA mode to read Station MAC
  WiFi.disconnect();     // Don't actually connect to any network

  Serial.println();
  Serial.println("=========================================");
  Serial.println("        ESP32 MAC ADDRESS FINDER");
  Serial.println("=========================================");
}

void loop() {
  Serial.print("Station MAC Address: ");
  Serial.println(WiFi.macAddress());
  Serial.println("-----------------------------------------");
  delay(5000);
}
