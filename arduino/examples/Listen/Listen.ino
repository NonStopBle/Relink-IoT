/*
 * Listen -- print what the dashboard or other programs write.
 *
 * Dashboard setup:  V0 Float, V1 Integer, V2 String, V3 ByteArray
 * Pairs with:       the C++ / Python "basic" examples
 * Board:            ESP32 or ESP8266
 * You'll see:       "[V1] 1" in the Serial monitor for each write, and writes/s every 5 s.
 */

#ifdef ESP8266
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif
#include <RelinkIoT.h>

// ── 1. Your settings (from the device page) ──
#define WIFI_SSID      "YourSSID"
#define WIFI_PASS      "YourPassword"
#define DEVICE_TOKEN   "your-device-token-here"
#define DEVICE_ENC_KEY "your-64-hex-char-key"   // from the device page; "" = no encryption
#define RELINK_SERVER  "relink-iot.com"

// ── 2. WiFi: the sketch connects, Relink follows (registers on IP, pauses on drop) ──
#ifdef ESP8266
WiFiEventHandler wifiGotIPHandler, wifiDisconnectHandler;   // keep global: handlers die with these
#endif

// Lambdas, not functions: the .ino preprocessor would prototype both boards'
// handler signatures and break the build on the other one.
void startWiFi() {
#ifdef ESP8266
    wifiGotIPHandler = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP&) {
        Serial.printf("[wifi] IP %s\n", WiFi.localIP().toString().c_str());
    });
    wifiDisconnectHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& e) {
        Serial.printf("[wifi] lost (reason %d), reconnecting\n", e.reason);
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASS);
    });
#else
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
        Serial.printf("[wifi] IP %s\n", WiFi.localIP().toString().c_str());
    }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
        Serial.printf("[wifi] lost (reason %d), reconnecting\n", info.wifi_sta_disconnected.reason);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
    }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
#endif
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

static uint32_t received = 0;

// What the dashboard or another program (C++/Python) writes, by pin.
RELINK_WRITE(V0) {
    received++;
    Serial.printf("[V0] %.3f\n", param.asFloat());
}

RELINK_WRITE(V1) {
    received++;
    Serial.printf("[V1] %d\n", param.asInt());
}

RELINK_WRITE(V2) {
    received++;
    Serial.printf("[V2] \"%s\"\n", param.asString().c_str());
}

RELINK_WRITE(V3) {
    received++;
    Serial.printf("[V3] %u bytes:", param.length());
    for (uint8_t i = 0; i < param.length(); i++) Serial.printf(" %02x", param.asBytes()[i]);
    Serial.println();
}

void setup() {
    Serial.begin(115200);
    Relink.setDebug(true);
    startWiFi();
    Relink.begin(DEVICE_TOKEN, DEVICE_ENC_KEY, RELINK_SERVER);
}

void loop() {
    Relink.run();

    static unsigned long last = 0;
    if (millis() - last >= 5000) {
        Serial.printf("[listen] %.1f writes/s, %s\n", received / ((millis() - last) / 1000.0f),
                      Relink.registered() ? "registered" : "offline");
        received = 0;
        last = millis();
    }
}
