/*
 * ByteArray -- pack several readings into one write.
 *
 * Dashboard setup:  V2 ByteArray (record out), V3 ByteArray (bytes in)
 * Pairs with:       the C++ / Python "bytes" examples (they decode V2)
 * Board:            ESP32 or ESP8266
 * Sends:            V2 = [ax int16][ay int16][az int16][status uint8], every 200 ms
 * You'll see:       bytes written to V3 printed as hex in the Serial monitor.
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
#define SERVER_IP      "192.168.1.100"

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

// Bytes from the dashboard / C++ / Python on V3
RELINK_WRITE(V3) {
    const uint8_t* b = param.asBytes();
    Serial.printf("[V3] %u bytes:", param.length());
    for (uint8_t i = 0; i < param.length(); i++) Serial.printf(" %02x", b[i]);
    Serial.println();
}

void setup() {
    Serial.begin(115200);
    Relink.setDebug(true);
    startWiFi();
    Relink.begin(DEVICE_TOKEN, DEVICE_ENC_KEY, SERVER_IP);

}

void loop() {
    Relink.run();

    static unsigned long last = 0;
    if (millis() - last >= 200) {
        last = millis();
        float t = millis() / 1000.0f;
        int16_t ax = (int16_t)(1000 * sinf(t)), ay = (int16_t)(1000 * cosf(t)), az = 981;
        uint8_t rec[7];
        memcpy(rec, &ax, 2); memcpy(rec + 2, &ay, 2); memcpy(rec + 4, &az, 2);   // ESP32/ESP8266 are little-endian
        rec[6] = Relink.registered() ? 1 : 0;
        Relink.virtualWrite(V2, rec, sizeof(rec));
    }
}
