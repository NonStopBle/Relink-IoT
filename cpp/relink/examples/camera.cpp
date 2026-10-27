// Camera -- receive a device's live camera, peer-to-peer.
//
// Dashboard setup:  V10 Camera, V11 ByteArray
// Pairs with:       the Arduino "CameraStream" example
// Run:              ./relink_camera [seconds] [latest.jpg] [settings]
//                   e.g. ./relink_camera 60 latest.jpg "fs=QVGA;q=20;fps=15"
// You'll see:       "15.0 fps  12.3 KB/frame  dropped 0" every 2 s; with a
//                   file name, the newest frame is saved there.
#include <relink/relink.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include "settings.hpp"   // 1. Your settings live here (camera needs a KEY: P2P mesh)

static const uint8_t STREAM_PIN = 10, SETTINGS_PIN = 11;

int main(int argc, char** argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 1 << 30;
    const char* out = argc > 2 && *argv[2] ? argv[2] : nullptr;
    const char* settings = argc > 3 ? argv[3] : nullptr;

    // 2. Connect
    relink::Client r(RELINK_TOKEN_, RELINK_KEY_, RELINK_SERVER_);

    // 3. Each frame arrives as JPEG bytes
    std::atomic<long> frames{0}, bytes{0}, gaps{0};
    int last = -1;
    r.onCamera(STREAM_PIN, [&](const uint8_t* jpeg, size_t len, uint16_t fid) {
        if (last >= 0 && (uint16_t)(fid - last) > 1) gaps += (uint16_t)(fid - last) - 1;
        last = fid;
        frames++; bytes += (long)len;
        if (out) { if (FILE* f = fopen(out, "wb")) { fwrite(jpeg, 1, len, f); fclose(f); } }
    });

    r.start();
    if (settings) r.virtualWrite(SETTINGS_PIN, std::string(settings));

    // 4. Stats every 2 s
    for (int s = 0; s < secs; s += 2) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        long n = frames.exchange(0), b = bytes.exchange(0);
        printf("%5.1f fps  %4.1f KB/frame  dropped %ld | ", n / 2.0, n ? b / 1024.0 / n : 0.0, gaps.exchange(0));
        printStatus(r);
    }
    r.stop();
}
