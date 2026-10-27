/**
 * BasicSensor — RelinkIoT Arduino example
 *
 * Reads analog sensor on A0, sends to V0 every second.
 * Receives slider value on V1 to control LED brightness.
 *
 * Hardware: ESP32 or ESP8266
 */

#include <RelinkIoT.h>

#define WIFI_SSID     "YourSSID"
#define WIFI_PASS     "YourPassword"
#define DEVICE_TOKEN  "your-device-token-here"
#define DEVICE_ENC_KEY "your-64-hex-char-key"   // from the device page; "" = no encryption
#define SERVER_IP     "192.168.1.100"
#define SERVER_PORT   7445

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
    Relink.config(WIFI_SSID, WIFI_PASS);
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
