/*
 * CameraStreamWithMic -- live camera and live sound from one board, peer-to-peer.
 *
 * Dashboard setup:  V10 Camera -> Live Camera widget, V11 ByteArray (its settings pin),
 *                   V0 Audio -> Audio (P2P) widget: tap to listen, hold the mic to talk.
 * Board:            Seeed XIAO ESP32S3 Sense (camera + PDM mic: CLK GPIO42, DATA GPIO41).
 *                   ESP32-CAM has no mic; wire an I2S mic and change the pins to use it.
 * PSRAM:            REQUIRED. Arduino IDE: Tools > PSRAM > "OPI PSRAM".
 * Build flags:      -DRELINK_CAMERA_STREAM -DRELINK_AUDIO_STREAM -DBOARD_HAS_PSRAM
 *                   Arduino IDE: uncomment both #defines in RelinkIoT.h (a #define here
 *                   does not reach the library).
 * Flash:            XIAO ESP32S3 (8 MB): RelinkIoT/partitions/relink_4mb_ota.csv works.
 * Speaker (option): an I2S amp such as MAX98357: set SPK_BCLK / SPK_LRC / SPK_DIN below.
 * You'll see:       video in the camera widget, sound in the audio widget. The camera is
 *                   only read while someone watches, the mic only while someone listens.
 *
 * Why the mic has its own task: grabbing a big camera frame can take longer than the
 * mic's buffer lasts, and audio would drop. The task reads 20 ms blocks into a queue;
 * loop() sends whatever is queued.
 */
#define CAMERA_MODEL_XIAO_ESP32S3

#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <ESP_I2S.h>
#include <RelinkIoT.h>
#include "camera_pins.h"

#if !defined(RELINK_CAMERA_STREAM) || !defined(RELINK_AUDIO_STREAM)
#error "Build with RELINK_CAMERA_STREAM and RELINK_AUDIO_STREAM (see the comment at the top)"
#endif

// ── 1. Your settings (from the device page) ──
#ifndef WIFI_SSID
#define WIFI_SSID      "your-ssid"
#define WIFI_PASS      "your-password"
#define DEVICE_TOKEN   "your-device-token"
#define DEVICE_ENC_KEY "your-64-hex-char-key"
#define SERVER_IP      "relink-iot.com"
#endif
#define MIC_CLK   42
#define MIC_DATA  41
#define MIC_GAIN  8
// #define SPK_BCLK 7        // I2S amp (MAX98357): uncomment and set your pins
// #define SPK_LRC  8
// #define SPK_DIN  9

struct AudioBlock { int16_t pcm[160]; };   // 20 ms at 8 kHz
I2SClass mic;
QueueHandle_t micQueue;
#ifdef SPK_DIN
I2SClass speaker;
#endif

// ── 2. WiFi: the sketch connects, Relink follows ──
void startWiFi() {
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
        Serial.printf("[wifi] IP %s\n", WiFi.localIP().toString().c_str());
    }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
        WiFi.begin(WIFI_SSID, WIFI_PASS);
    }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

static bool initCamera() {
    if (!psramFound()) {
        Serial.println("[cam] PSRAM is off -- Arduino IDE: Tools > PSRAM > \"OPI PSRAM\"; PlatformIO: -DBOARD_HAS_PSRAM");
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
    c.jpeg_quality = 14;
    c.fb_count     = 3;
    c.fb_location  = CAMERA_FB_IN_PSRAM;
    c.grab_mode    = CAMERA_GRAB_LATEST;
    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) { Serial.printf("[cam] init failed 0x%x\n", err); return false; }
    esp_camera_sensor_get()->set_framesize(esp_camera_sensor_get(), FRAMESIZE_VGA);   // until V11 says otherwise
    return true;
}

// Reads the mic only while someone listens, removes the PDM DC offset, queues 20 ms blocks.
void micTask(void*) {
    int32_t dc = 0;
    AudioBlock b;
    for (;;) {
        if (!Relink.audioWanted(V0)) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        size_t n = mic.readBytes((char*)b.pcm, sizeof b.pcm) / 2;
        if (n != 160) continue;
        for (size_t i = 0; i < n; i++) {
            dc += (b.pcm[i] * 256 - dc) / 512;
            b.pcm[i] = (int16_t)(b.pcm[i] - dc / 256);
        }
        xQueueSend(micQueue, &b, 0);    // queue full = loop() is behind: drop, don't wait
    }
}

// ── 3. Handlers ──
// Camera settings from the dashboard widget -> sensor.
RELINK_WRITE(V11) {
    Relink.cameraApply(param);
}

// Dashboard -> board (push-to-talk), 20 ms per call.
void onTalk(const int16_t* pcm, size_t n) {
#ifdef SPK_DIN
    speaker.write((const uint8_t*)pcm, n * 2);
#else
    (void)pcm; (void)n;
#endif
}

void setup() {
    Serial.begin(115200);
    delay(300);
    if (!initCamera()) { while (true) delay(1000); }

    mic.setPinsPdmRx(MIC_CLK, MIC_DATA);
    if (!mic.begin(I2S_MODE_PDM_RX, 8000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO))
        Serial.println("[mic] I2S start failed");
#ifdef SPK_DIN
    speaker.setPins(SPK_BCLK, SPK_LRC, SPK_DIN);
    speaker.begin(I2S_MODE_STD, 8000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
#endif
    micQueue = xQueueCreate(10, sizeof(AudioBlock));
    xTaskCreatePinnedToCore(micTask, "mic", 4096, nullptr, 5, nullptr, 1);

    Relink.onAudio(V0, onTalk);
    Relink.setDebug(true);
    startWiFi();
    Relink.begin(DEVICE_TOKEN, DEVICE_ENC_KEY, SERVER_IP);
}

// ── 4. Loop ──
void loop() {
    Relink.run();

    AudioBlock b;
    while (xQueueReceive(micQueue, &b, 0) == pdTRUE)
        Relink.audioWrite(V0, b.pcm, 160, MIC_GAIN);

    if (Relink.virtualImageWanted()) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb && !Relink.virtualImageWrite(V10, fb)) esp_camera_fb_return(fb);
    }
}
