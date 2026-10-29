/*
 * RelinkConfig -- let Relink handle WiFi for you, no WiFi code at all.
 *
 * Dashboard setup:  V0 Float (uptime), V1 Integer (printed)
 * Board:            ESP32 or ESP8266
 * Use it when:      you just want it working. Relink.config() hands Relink the
 *                   credentials: it calls WiFi.begin() (non-blocking) and retries
 *                   every 10 s while WiFi is down. The other examples use WiFi
 *                   events instead, so the sketch owns the connection.
 * Flash (ESP32):    use the bigger OTA partition table (WebRTC on 53% instead of 83%):
 *                   Arduino IDE: copy RelinkIoT/partitions/relink_4mb_ota.csv here as
 *                   partitions.csv, and Tools > Partition Scheme > Minimal SPIFFS.
 *                   PlatformIO: board_build.partitions. First upload over USB.
 *                   See "ESP32 flash size" in the README.
 */

#include <RelinkIoT.h>

// ── 1. Your settings (from the device page) ──
#define WIFI_SSID      "YourSSID"
#define WIFI_PASS      "YourPassword"
#define DEVICE_TOKEN   "your-device-token-here"
#define DEVICE_ENC_KEY "your-64-hex-char-key"   // from the device page; "" = no encryption
#define SERVER_IP      "192.168.1.100"

RELINK_WRITE(V1) {
    Serial.printf("[V1] %d\n", param.asInt());
}

void setup() {
    Serial.begin(115200);
    Relink.setDebug(true);
    Relink.config(WIFI_SSID, WIFI_PASS);   // Relink connects and reconnects WiFi
    Relink.onConnect([]()    { Serial.println("[relink] WiFi up"); });
    Relink.onRegistered([]() { Serial.println("[relink] registered"); });
    Relink.begin(DEVICE_TOKEN, DEVICE_ENC_KEY, SERVER_IP);   // returns immediately
}

void loop() {
    Relink.run();

    static unsigned long last = 0;
    if (millis() - last >= 1000) {
        last = millis();
        Relink.virtualWrite(V0, (float)(millis() / 1000.0f));
    }
}
