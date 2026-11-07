// Audio -- listen to a device's mic and talk back, peer-to-peer.
//
// Dashboard setup:  V0 Audio
// Pairs with:       the Arduino "AudioStream" example (XIAO ESP32S3 Sense mic)
// Run:              ./relink_audio [seconds] [heard.wav] [--tone]
//                   e.g. ./relink_audio 15 heard.wav --tone
// You'll see:       "50 packets/s  level 52 dB" every second and the device's
//                   sound in heard.wav (8 kHz mono). With --tone the device
//                   prints "[talk] level 77 dB, 50 packets/s".
#include <relink/relink.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include "settings.hpp"   // 1. Your settings live here (audio needs a KEY: P2P mesh)

static void writeWav(const char* path, const std::vector<int16_t>& s) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t data = (uint32_t)s.size() * 2, riff = 36 + data, fmt = 16, rate = 8000, bps = 16000;
    uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&bps, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(s.data(), 2, s.size(), f);
    fclose(f);
}

int main(int argc, char** argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    const char* out = argc > 2 ? argv[2] : "heard.wav";
    bool tone = argc > 3 && !strcmp(argv[3], "--tone");
    const uint8_t PIN = 0;

    // 2. Connect
    relink::Client r(RELINK_TOKEN_, RELINK_KEY_, RELINK_SERVER_);

    // 3. Each 20 ms of the device's mic arrives as 160 samples
    std::mutex mu;
    std::vector<int16_t> heard;
    std::atomic<long> packets{0};
    double sq = 0; long count = 0;
    r.onAudio(PIN, [&](const int16_t* pcm, size_t n) {
        std::lock_guard<std::mutex> lk(mu);
        heard.insert(heard.end(), pcm, pcm + n);
        for (size_t i = 0; i < n; i++) sq += (double)pcm[i] * pcm[i];
        count += (long)n; packets++;
    });
    r.start();

    // 4. Talk (optional tone) every 20 ms, stats every second
    int16_t buf[160];
    long t = 0;
    for (int tick = 0; tick < secs * 50; tick++) {
        if (tone && !r.peers().empty()) {
            for (int i = 0; i < 160; i++, t++) buf[i] = (int16_t)(9000 * sin(2 * M_PI * 440 * t / 8000.0));
            r.audioWrite(PIN, buf, 160);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (tick % 50 == 49) {
            std::lock_guard<std::mutex> lk(mu);
            printf("%3ld packets/s  level %3.0f dB  ", packets.exchange(0), count ? 20 * log10(sqrt(sq / count) + 1) : 0.0);
            sq = 0; count = 0;
            printStatus(r);
            fflush(stdout);
        }
    }
    std::lock_guard<std::mutex> lk(mu);
    writeWav(out, heard);
    printf("%s: %.1f s\n", out, heard.size() / 8000.0);
    return 0;
}
