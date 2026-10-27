/*
 * WiFiEvents -- your sketch manages WiFi, Relink just follows it.
 *
 * Dashboard setup:  V0 Float (uptime), V1 Integer (printed)
 * Board:            ESP32 or ESP8266 (each core has its own event API)
 * Use it when:      you need a static IP, WiFiManager or your own reconnect logic.
 *                   No Relink.config(): Relink registers whenever WiFi has an IP,
 *                   pauses on a drop and re-registers by itself.
 */

#ifdef ESP8266
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif
#include <RelinkIoT.h>

// ── 1. Your settings (from the device page) ──
const char* WIFI_SSID = "YourSSID";
const char* WIFI_PASS = "YourPassword";

#define DEVICE_TOKEN   "your-device-token-here"
#define DEVICE_ENC_KEY "your-64-hex-char-key"   // from the device page; "" = no encryption
#define RELINK_SERVER  "relink-iot.com"

#ifdef ESP8266
// ESP8266: handlers live only as long as these objects -- keep them global.
WiFiEventHandler wifiConnectHandler, wifiGotIPHandler, wifiDisconnectHandler;
#endif

RELINK_WRITE(V1) {
    Serial.printf("[V1] %d\n", param.asInt());
}

void setup() {
    Serial.begin(115200);

    WiFi.disconnect(true);   // drop any stored config
    delay(1000);
    // Lambdas, not functions: the .ino preprocessor would prototype both
    // boards' handler signatures and break the build on the other one.
#ifdef ESP8266
    wifiConnectHandler = WiFi.onStationModeConnected([](const WiFiEventStationModeConnected&) {
        Serial.println("[wifi] associated with access point");
    });
    wifiGotIPHandler = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP&) {
        Serial.print("[wifi] IP address: ");
        Serial.println(WiFi.localIP());
    });
    wifiDisconnectHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& event) {
        Serial.printf("[wifi] lost connection (reason %d), reconnecting\n", event.reason);
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASS);
    });
#else
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
        Serial.println("[wifi] associated with access point");
    }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_CONNECTED);
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
        Serial.print("[wifi] IP address: ");
        Serial.println(WiFi.localIP());
    }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
        Serial.printf("[wifi] lost connection (reason %d), reconnecting\n", info.wifi_sta_disconnected.reason);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
    }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
#endif
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // No Relink.config(): Relink follows whatever WiFi state the sketch sets up.
    Relink.onRegistered([]() { Serial.println("[relink] registered"); });
    Relink.onDisconnect([]() { Serial.println("[relink] paused, waiting for WiFi"); });
    Relink.begin(DEVICE_TOKEN, DEVICE_ENC_KEY, RELINK_SERVER);   // returns immediately
}

void loop() {
    Relink.run();

    static unsigned long last = 0;
    if (millis() - last >= 1000) {
        last = millis();
        Relink.virtualWrite(V0, (float)(millis() / 1000.0f));
    }
}
