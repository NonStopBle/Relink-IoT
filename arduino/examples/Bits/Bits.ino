/*
 * Bits -- an 8-channel relay board on one UInt8 pin.
 *
 * Dashboard setup:  V5 UInt8 (0–255). Add 8 Switch widgets on V5, Bit 0 to Bit 7
 *                   (one per relay channel). Each switch flips only its own bit.
 * Pairs with:       the C++ / Python "bits" examples (same V5, they blink channel 5 = bit 4)
 * Board:            ESP32 + 8-channel relay module (ESP8266 builds; change the GPIOs,
 *                   untested on hardware)
 * Wiring:           IN1–IN8 -> GPIO 16, 17, 18, 19, 21, 22, 23, 25. Most relay modules
 *                   are active LOW: keep RELAY_ACTIVE_LOW true, set false for active HIGH.
 * You'll see:       the relays follow the switches; "[V5] 0b00000101 (5) CH1 CH3" in the
 *                   Serial monitor. RelinkBits keeps the one pin value: read it as a
 *                   number (relays.u8) or as bits (relays.asBit(2)).
 * Flash (ESP32):    use the bigger OTA partition table (WebRTC on 53% instead of 83%):
 *                   Arduino IDE: copy RelinkIoT/partitions/relink_4mb_ota.csv here as
 *                   partitions.csv, and Tools > Partition Scheme > Minimal SPIFFS.
 *                   PlatformIO: board_build.partitions. First upload over USB.
 *                   See "ESP32 flash size" in the README.
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

// ── 3. Handlers ──
const uint8_t RELAY_PINS[8] = {16, 17, 18, 19, 21, 22, 23, 25};   // CH1–CH8 = bit 0–7
const bool    RELAY_ACTIVE_LOW = true;                            // most relay modules
RelinkBits relays;   // V5's value, owned by the sketch; the library stores no bits

void applyRelays() {
    for (uint8_t ch = 0; ch < 8; ch++)
        digitalWrite(RELAY_PINS[ch], relays.asBit(ch) != RELAY_ACTIVE_LOW ? HIGH : LOW);
}

// The dashboard sends the whole byte; keep it so our own changes don't undo its bits.
RELINK_WRITE(V5) {
    relays = param;
    applyRelays();
    Serial.printf("[V5] 0b");
    for (int i = 7; i >= 0; i--) Serial.print(relays.asBit(i));
    Serial.printf(" (%u)", relays.u8);
    for (uint8_t ch = 0; ch < 8; ch++) if (relays.asBit(ch)) Serial.printf(" CH%u", ch + 1);
    Serial.println();
}

// Switch one channel from the sketch and tell the dashboard (sends the whole byte).
void setRelay(uint8_t ch, bool on) {
    relays.setBit(ch, on);
    applyRelays();
    Relink.virtualWrite(V5, relays);   // UInt8 datastream -> 1 byte
}

void setup() {
    Serial.begin(115200);
    applyRelays();                     // all off before the pins become outputs
    for (uint8_t p : RELAY_PINS) pinMode(p, OUTPUT);
    Relink.setDebug(true);
    startWiFi();
    Relink.begin(DEVICE_TOKEN, DEVICE_ENC_KEY, SERVER_IP);
}

// ── 4. Loop ──
void loop() {
    Relink.run();

    // Your own logic can switch channels too, e.g. CH8 from the BOOT button:
    static bool lastBtn = false;
    bool btn = digitalRead(0) == LOW;
    if (btn && !lastBtn) setRelay(7, !relays.asBit(7));   // toggle CH8 on press
    lastBtn = btn;
}
