/**
 * RelinkIoT Arduino Library (precompiled) -- ESP32 family / ESP8266
 * ESP8266: builds, but not yet tested on a real board.
 *
 *   Relink.config("ssid", "password");
 *   Relink.begin(DEVICE_TOKEN, DEVICE_ENC_KEY, "server-ip");
 *
 *   void loop() {
 *     Relink.run();
 *     Relink.virtualWrite(V0, analogRead(A0));
 *   }
 *
 *   RELINK_WRITE(V1) { int v = param.asInt(); }
 *
 * Built with OTA and encryption on every chip, the direct P2P (WebRTC)
 * channel on ESP32 chips, and camera streaming (virtualImageWrite /
 * cameraApply) on chips with a camera interface: ESP32, ESP32-S2, ESP32-S3.
 * Camera code a sketch never calls is dropped at link time.
 */
#pragma once

#if !defined(ESP32) && !defined(ESP8266)
  #error "RelinkIoT supports ESP32 and ESP8266 only"
#endif

#include "RelinkTypes.h"
#include <type_traits>
#if defined(ESP32) && !defined(RELINK_CAMERA_STREAM) && __has_include(<esp_camera.h>) && \
    (defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3))
  #define RELINK_CAMERA_STREAM 1
#endif
// Core libraries the precompiled code links against: included here so the
// Arduino IDE / PlatformIO build them into the sketch.
#ifdef ESP32
  #include <WiFi.h>
  #include <WiFiUdp.h>
  #include <HTTPClient.h>
  #include <Update.h>
  #include <Preferences.h>
  #include <EEPROM.h>
#else
  #include <ESP8266WiFi.h>
  #include <WiFiUdp.h>
  #include <ESP8266HTTPClient.h>
  #include <Updater.h>
  #include <EEPROM.h>
#endif
#ifdef RELINK_CAMERA_STREAM
  #include <esp_camera.h>
#endif

#define RELINK_HAS_ENCRYPTION 1
#ifdef ESP32
  #define RELINK_HAS_WEBRTC 1
#endif

#define RELINK_API_PORT        8000    // FastAPI port on the same host as the relay
#define RELINK_WEBRTC_PORT     17453   // local UDP port for the P2P data channel
#define RELINK_CLAIM_TOKEN_LEN 33      // claim() tokenOut buffer size
#define RELINK_CLAIM_KEY_LEN   65      // claim() keyHexOut buffer size

class RelinkIoT {
public:
    // WiFi + server config -- call before begin()
    void config(const char* ssid, const char* password);

    // Connect and start -- call in setup(). encKeyHex is the 64-hex-char key
    // from the device page; blank ("" / nullptr) = no encryption.
    void begin(const char* token, const char* encKeyHex, const char* server, uint16_t port = 7445);
    // Legacy form: plaintext unless enableEncryption() was called first.
    void begin(const char* token, const char* server, uint16_t port = 7445);

    // Sends the pending virtualWrite() batch now instead of at the next run().
    void flush();

    // OTA config (optional, call before begin())
    void configOTA(const char* apiBase = nullptr);
    void enableOTA(bool enable = true);

    // One-time bootstrap from the dashboard's single-use claim code. Fills
    // tokenOut / keyHexOut / checksumOut; persist them yourself.
    bool claim(const char* claimCode, const char* apiBase,
               char* tokenOut, size_t tokenOutLen,
               char* keyHexOut, size_t keyHexOutLen,
               uint32_t* checksumOut);

    // Older way to pass the key (call before the 3-argument begin()).
    void enableEncryption(uint32_t deviceId, const char* keyHex);
    bool secure() const;

#ifdef RELINK_HAS_WEBRTC
    // Direct browser<->device data channel. run() drives it automatically.
    bool webrtcBegin(uint16_t udpPort, const char* local_ice_pwd, const char* apiBase = nullptr);
    void webrtcPoll();
    bool webrtcSend(const uint8_t* data, size_t len);
    bool webrtcConnected();
    bool webrtcConnecting();
    const char* webrtcFingerprint();
    void webrtcOnOpen(RelinkCallback cb);
    void webrtcOnMessage(std::function<void(const uint8_t*, size_t)> cb);
#endif

#ifdef RELINK_CAMERA_STREAM
    // Sends one JPEG on V<pin> to whoever is watching (P2P only, never
    // relayed). Non-blocking; the frame goes back to the driver once sent:
    //   if (Relink.virtualImageWanted()) {
    //       camera_fb_t* fb = esp_camera_fb_get();
    //       if (fb && !Relink.virtualImageWrite(V10, fb)) esp_camera_fb_return(fb);
    //   }
    bool virtualImageWrite(uint8_t pin, camera_fb_t* fb);
    // Any other buffer, non-blocking: release(ctx) runs once `data` is no longer read.
    bool virtualImageWrite(uint8_t pin, const uint8_t* data, size_t len, void (*release)(void*), void* ctx);
    // Blocking: returns once the image is sent.
    bool virtualImageWrite(uint8_t pin, const uint8_t* data, size_t len);
    bool virtualImageWanted();
    bool virtualImageBusy() const;
    // Camera widget settings -> sensor:  RELINK_WRITE(V11) { Relink.cameraApply(param); }
    void cameraApply(const RelinkParam& param);
#endif

    // Main loop -- call every iteration of loop()
    void run();

    bool connected();
    bool registered();

    // VPin write (device -> server -> dashboard)
    void virtualWrite(uint8_t pin, int16_t v);
    void virtualWrite(uint8_t pin, int32_t v);
    // Any other integer (int, long, unsigned, bool, millis()): on ESP32 int32_t
    // is `long`, so a plain int matched no overload exactly and
    // virtualWrite(V0, 5) was ambiguous. Sent as int32.
    template <typename T, typename std::enable_if<std::is_integral<T>::value
        && !std::is_same<T, int16_t>::value && !std::is_same<T, int32_t>::value, int>::type = 0>
    void virtualWrite(uint8_t pin, T v) { virtualWrite(pin, (int32_t)v); }
    void virtualWrite(uint8_t pin, float v);
    void virtualWrite(uint8_t pin, const char* s);
    void virtualWrite(uint8_t pin, const String& s);
    void virtualWrite(uint8_t pin, const uint8_t* data, uint8_t len);   // ByteArray datastream

    // VPin read callbacks (dashboard -> server -> device)
    void onVirtualPin(uint8_t pin, RelinkCallbackInt16 cb);
    void onVirtualPin(uint8_t pin, RelinkCallbackInt32 cb);
    void onVirtualPin(uint8_t pin, RelinkCallbackFloat cb);
    void onVirtualPin(uint8_t pin, RelinkCallbackString cb);
    void onVirtualPin(uint8_t pin, RelinkCallbackBytes cb);

    // Lifecycle events
    void onConnect(RelinkCallback cb);
    void onDisconnect(RelinkCallback cb);
    void onRegistered(RelinkCallback cb);

    // Diagnostics
    void     setDebug(bool enable = true);
    uint32_t getUptime();
    float    getLatency();
    int32_t  getFirmwareVersion();
};

extern RelinkIoT Relink;
