/*
 * BasicSensor -- send a reading to V0, react when V1 / V2 change.
 *
 * Dashboard setup:  V0 Float   -> Label or Gauge widget
 *                   V1 Integer -> Slider 0-255 (LED brightness)
 *                   V2 Float   -> any control (printed)
 * Board:            ESP32 or ESP8266
 * You'll see:       A0 voltage on the dashboard; moving the Slider dims the LED.
 */

#ifdef ESP8266
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif
#include <RelinkIoT.h>

// ── 1. Your settings (from the device page) ──
#define WIFI_SSID     "YourSSID"
#define WIFI_PASS     "YourPassword"
#define DEVICE_TOKEN  "your-device-token-here"
#define DEVICE_ENC_KEY "your-64-hex-char-key"   // from the device page; "" = no encryption
#define SERVER_IP     "192.168.1.100"
#define SERVER_PORT   7445

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

#define LED_PIN       2   // built-in LED on most ESP32/ESP8266 boards

// Called when server/dashboard sets V1 (e.g. a slider widget)
RELINK_WRITE(V1) {
    int brightness = param.asInt();  // 0–255
    analogWrite(LED_PIN, brightness);
    Serial.printf("[V1] brightness = %d\n", brightness);
}

void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);

    Relink.setDebug(true);
    startWiFi();
    Relink.begin(DEVICE_TOKEN, DEVICE_ENC_KEY, SERVER_IP, SERVER_PORT);

    Relink.onRegistered([]() {
        Serial.println("[relink] device registered — sending data");
    });

    // Lambda-style callback for V2 (alternative to RELINK_WRITE macro)
    Relink.onVirtualPin(V2, RelinkCallbackFloat([](float v) {
        Serial.printf("[V2] float value = %.2f\n", v);
    }));
}

void loop() {
    Relink.run();

    // Send sensor reading to V0 every 1 second
    static unsigned long lastSend = 0;
    if (millis() - lastSend >= 1000) {
        lastSend = millis();
        float voltage = analogRead(A0) * (3.3f / 4095.0f);  // ESP32 12-bit ADC
        Relink.virtualWrite(V0, voltage);
    }
}
