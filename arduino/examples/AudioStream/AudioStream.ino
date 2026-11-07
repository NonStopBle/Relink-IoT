/*
 * AudioStream -- live sound from the board's mic to the dashboard (P2P).
 *
 * Dashboard setup:  V0 Audio datastream. Add an "Audio (P2P)" widget on V0: tap to listen,
 *                   hold the mic button to talk to the board.
 * Board:            Seeed XIAO ESP32S3 Sense (PDM mic: CLK GPIO42, DATA GPIO41). Any ESP32 with
 *                   an I2S/PDM mic works: change the pins and the mic.begin() mode.
 * Speaker (option): an I2S amp such as MAX98357: set SPK_BCLK / SPK_LRC / SPK_DIN below.
 *                   Without one, the board prints the level of what you say instead.
 * You'll see:       "[audio] dashboard listening on V0" in the Serial monitor, then the sound in
 *                   the browser. The mic is only read while someone listens. While you hold
 *                   talk: "[talk] level 41 dB, 50 packets/s".
 * Flash:            needs RELINK_AUDIO_STREAM. PlatformIO: build_flags = -DRELINK_AUDIO_STREAM.
 *                   Arduino IDE: uncomment "#define RELINK_AUDIO_STREAM" in RelinkIoT.h.
 *                   Use the bigger OTA partition table (see "ESP32 flash size" in the README).
 */

#include <WiFi.h>
#include <ESP_I2S.h>
#include <RelinkIoT.h>

// ── 1. Your settings (from the device page) ──
#ifndef WIFI_SSID
#define WIFI_SSID      "YourSSID"
#define WIFI_PASS      "YourPassword"
#define DEVICE_TOKEN   "your-device-token-here"
#define DEVICE_ENC_KEY "your-64-hex-char-key"
#define SERVER_IP      "relink-iot.com"
#endif
#define MIC_CLK   42
#define MIC_DATA  41
#define MIC_GAIN  8          // small MEMS mics are quiet; the widget can boost more
// #define SPK_BCLK 7        // I2S amp (MAX98357): uncomment and set your pins
// #define SPK_LRC  8
// #define SPK_DIN  9

I2SClass mic;
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

void onTalk(const int16_t* pcm, size_t n);

void setup() {
    Serial.begin(115200);
    mic.setPinsPdmRx(MIC_CLK, MIC_DATA);
    if (!mic.begin(I2S_MODE_PDM_RX, 8000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO))
        Serial.println("[mic] I2S start failed");
#ifdef SPK_DIN
    speaker.setPins(SPK_BCLK, SPK_LRC, SPK_DIN);
    speaker.begin(I2S_MODE_STD, 8000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
#endif
    Relink.onAudio(V0, onTalk);
    Relink.setDebug(true);
    startWiFi();
    Relink.begin(DEVICE_TOKEN, DEVICE_ENC_KEY, SERVER_IP);
}

// ── 3. Handlers ──
// Dashboard -> board (push-to-talk): 20 ms per call.
uint32_t talkPackets = 0;
double   talkSq = 0;
uint32_t talkN = 0;

void onTalk(const int16_t* pcm, size_t n) {
#ifdef SPK_DIN
    speaker.write((const uint8_t*)pcm, n * 2);
#endif
    talkPackets++;
    for (size_t i = 0; i < n; i++) talkSq += (double)pcm[i] * pcm[i];
    talkN += n;
}

void reportTalk() {
    static uint32_t last = 0;
    if (millis() - last < 1000) return;
    last = millis();
    if (talkPackets) Serial.printf("[talk] level %.0f dB, %u packets/s\n", 20 * log10(sqrt(talkSq / talkN) + 1), talkPackets);
    talkPackets = 0; talkSq = 0; talkN = 0;
}

// Mic -> dashboard, only while someone listens ──
void streamMic() {
    if (!Relink.audioWanted(V0)) return;
    int16_t buf[160];                                  // 20 ms at 8 kHz
    size_t n = mic.readBytes((char*)buf, sizeof buf) / 2;
    static int32_t dc = 0;                             // PDM mics sit off zero: remove the offset
    for (size_t i = 0; i < n; i++) {
        dc += (buf[i] * 256 - dc) / 512;
        buf[i] = (int16_t)(buf[i] - dc / 256);
    }
    Relink.audioWrite(V0, buf, n, MIC_GAIN);
}

// ── 4. Loop ──
void loop() {
    Relink.run();
    streamMic();
    reportTalk();
}
