/**
 * WiFiEvents -- RelinkIoT with WiFi managed by the sketch (ESP32).
 *
 * Relink never touches WiFi here: connect, reconnect, use a static IP or
 * WiFiManager -- whatever your project needs. Relink.run() notices when
 * WiFi gets an IP, sets its socket up and registers; after a drop it pauses
 * and re-registers on the next IP by itself.
 */
#include <WiFi.h>
#include <RelinkIoT.h>

const char* WIFI_SSID = "YourSSID";
const char* WIFI_PASS = "YourPassword";

#define DEVICE_TOKEN   "your-device-token-here"
#define DEVICE_ENC_KEY "your-64-hex-char-key"   // from the device page; "" = no encryption
#define SERVER_IP      "192.168.96.50"

void onWiFiConnected(WiFiEvent_t, WiFiEventInfo_t) {
    Serial.println("[wifi] associated with access point");
}

void onWiFiGotIP(WiFiEvent_t, WiFiEventInfo_t) {
    Serial.print("[wifi] IP address: ");
    Serial.println(WiFi.localIP());
}

void onWiFiDisconnected(WiFiEvent_t, WiFiEventInfo_t info) {
    Serial.printf("[wifi] lost connection (reason %d), reconnecting\n", info.wifi_sta_disconnected.reason);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

RELINK_WRITE(V1) {
    Serial.printf("[V1] %d\n", param.asInt());
}

void setup() {
    Serial.begin(115200);

    WiFi.disconnect(true);   // drop any stored config
    delay(1000);
    WiFi.onEvent(onWiFiConnected,    WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_CONNECTED);
    WiFi.onEvent(onWiFiGotIP,        WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.onEvent(onWiFiDisconnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // No Relink.config(): Relink follows whatever WiFi state the sketch sets up.
    Relink.onRegistered([]() { Serial.println("[relink] registered"); });
    Relink.onDisconnect([]() { Serial.println("[relink] paused, waiting for WiFi"); });
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
