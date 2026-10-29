/*
 * CameraStream -- live camera over Relink, peer-to-peer only.
 *
 * Dashboard setup:  V10 Camera -> Live Camera widget, V11 ByteArray (its settings pin)
 * Board:            XIAO ESP32S3 Sense or AI-Thinker ESP32-CAM
 * PSRAM:            REQUIRED. Arduino IDE: Tools > PSRAM > "OPI PSRAM" (XIAO ESP32S3 Sense)
 *                   or "Enabled" (ESP32-CAM). Without it the camera fails with
 *                   "frame buffer malloc failed".
 * Build flag:       -DRELINK_CAMERA_STREAM  (see "Needs" below)
 * Flash:            XIAO ESP32S3 (8 MB): default table is fine. ESP32-CAM (4 MB): use
 *                   RelinkIoT/partitions/relink_4mb_ota.csv, see "ESP32 flash size" in the README.
 * You'll see:       the video in the Live Camera widget, and in camera.py / relink_camera.
 *
 * ── How it works ──
 *
 * Frames go straight from the camera driver's buffer to whoever is watching:
 * the dashboard's "Live Camera (P2P)" widget over the direct WebRTC channel,
 * and C++/Python programs on the same token (Client::onCamera / on_camera)
 * over the encrypted mesh. Nothing is relayed through the server; frames are
 * only captured while someone watches. Sending is split across both cores.
 *
 * Datastreams: V10 type Camera (the image stream), V11 type ByteArray (the
 * widget's camera settings: resolution, quality, brightness, mirror, max
 * fps, ...). You capture; Relink.virtualImageWrite() sends; the settings
 * arrive in RELINK_WRITE(V11) like any other pin.
 *
 * Needs RELINK_CAMERA_STREAM for the library (source build only -- the
 *   precompiled library has it built in on ESP32/S2/S3):
 *   PlatformIO:  build_flags = -DRELINK_CAMERA_STREAM -DBOARD_HAS_PSRAM
 *   Arduino IDE: uncomment "#define RELINK_CAMERA_STREAM" in RelinkIoT.h
 *                (a #define here does not reach the library), and set
 *                Tools > PSRAM to "OPI PSRAM" (XIAO ESP32S3 Sense) or "Enabled" (ESP32-CAM).
 * Boards: XIAO ESP32S3 Sense, AI-Thinker ESP32-CAM (pick one below).
 */
#define CAMERA_MODEL_XIAO_ESP32S3
// #define CAMERA_MODEL_AI_THINKER

#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <RelinkIoT.h>
#include "camera_pins.h"

#ifndef RELINK_CAMERA_STREAM
#error "Build with RELINK_CAMERA_STREAM (see the comment at the top)"
#endif

// ── 1. Your settings (from the device page) ──
#define WIFI_SSID       "your-ssid"
#define WIFI_PASS       "your-password"
#define DEVICE_TOKEN    "your-device-token"
#define DEVICE_ENC_KEY  "your-64-hex-char-key"
#define SERVER_IP       "192.168.1.10"

// ── 2. WiFi: the sketch connects, Relink follows (registers on IP, pauses on drop) ──
void startWiFi() {
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
        Serial.printf("[wifi] IP %s\n", WiFi.localIP().toString().c_str());
    }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
        Serial.printf("[wifi] lost (reason %d), reconnecting\n", info.wifi_sta_disconnected.reason);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
    }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

static bool initCamera() {
    if (!psramFound()) {
        // UXGA frame buffers only fit in PSRAM.
        Serial.println("[cam] no PSRAM -- Arduino IDE: Tools > PSRAM > \"OPI PSRAM\" (XIAO ESP32S3 Sense)"
                       " or \"Enabled\" (ESP32-CAM); PlatformIO: -DBOARD_HAS_PSRAM");
        return false;
    }
    if (!psramFound()) {   // the frame buffers only fit in PSRAM
        Serial.println("[cam] PSRAM is off -- Arduino IDE: Tools > PSRAM > \"OPI PSRAM\" (XIAO ESP32S3 Sense)"
                       " or \"Enabled\" (ESP32-CAM); PlatformIO: -DBOARD_HAS_PSRAM");
        return false;
    }
    camera_config_t c = {};
    c.ledc_channel = LEDC_CHANNEL_0;
    c.ledc_timer   = LEDC_TIMER_0;
    c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
    c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
    c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM;
    c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
    c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
    c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
    c.xclk_freq_hz = 20000000;
    c.pixel_format = PIXFORMAT_JPEG;
    c.frame_size   = FRAMESIZE_UXGA;        // largest pickable: buffers are sized from it
    c.jpeg_quality = 14;                    // 0-63, lower = better
    c.fb_count     = 3;                     // one sending, one waiting, one capturing
    c.fb_location  = CAMERA_FB_IN_PSRAM;
    c.grab_mode    = CAMERA_GRAB_LATEST;    // always the newest frame, never a queued one
    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) { Serial.printf("[cam] init failed 0x%x\n", err); return false; }
    esp_camera_sensor_get()->set_framesize(esp_camera_sensor_get(), FRAMESIZE_VGA);   // until V11 says otherwise
    return true;
}

// Camera settings from the dashboard widget (or C++/Python) -> sensor.
RELINK_WRITE(V11) {
    Relink.cameraApply(param);
}

void setup() {
    Serial.begin(115200);
    delay(300);
    if (!initCamera()) { while (true) delay(1000); }

    Relink.setDebug(true);
    startWiFi();
    Relink.begin(DEVICE_TOKEN, DEVICE_ENC_KEY, SERVER_IP);
}

void loop() {
    Relink.run();

    // Capture only when someone watches, the previous frame is out and the
    // frame-rate limit allows. virtualImageWrite() doesn't block: a sender
    // task on the other core sends the frame and hands it back to the camera
    // driver, while this core is already capturing the next one.
    if (Relink.virtualImageWanted()) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb && !Relink.virtualImageWrite(V10, fb)) esp_camera_fb_return(fb);
    }
}
