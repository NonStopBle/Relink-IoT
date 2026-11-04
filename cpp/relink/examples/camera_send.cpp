// Camera send -- stream live video on a Camera pin, peer-to-peer, the same way
// an ESP32-CAM does. Frames come in as an MJPEG stream on stdin, so any source
// ffmpeg can read works (webcam, video file, RTSP camera, test pattern).
//
// Dashboard setup:  V10 Camera, V11 ByteArray (settings, optional)
// Watch with:       the dashboard's Live Camera widget on V10 (P2P data channel),
//                   or the C++ / Python "camera" examples (encrypted mesh). The
//                   newest viewer takes the stream.
// Needs:            an encryption key (video goes peer-to-peer, never through
//                   the relay); ffmpeg to make the MJPEG stream
// Run:              webcam:
//                     ffmpeg -loglevel error -f v4l2 -i /dev/video0 -vf scale=640:-2 \
//                       -r 15 -q:v 5 -f mjpeg - | ./relink_camera_send
//                   test pattern, no camera:
//                     ffmpeg -loglevel error -re -f lavfi -i testsrc=size=640x480:rate=15 \
//                       -q:v 5 -f mjpeg - | ./relink_camera_send
// You'll see:       "watching: yes  15.0 fps  24.1 KB/frame" every 2 s while a viewer
//                   watches; frames are skipped (not sent) while nobody does.
#include <relink/relink.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>
#include "settings.hpp"   // 1. Your settings live here

int main(int argc, char** argv) {
    // 2. Connect
    relink::Client r(RELINK_TOKEN_, RELINK_KEY_, RELINK_SERVER_);
    if (getenv("RELINK_DEBUG")) r.setDebug(true);   // P2P / camera log on stderr
    r.enableP2P();        // so the dashboard's Live Camera widget can watch
    r.start();
    const uint8_t STREAM_PIN = argc > 1 ? (uint8_t)atoi(argv[1]) : 10;   // ./relink_camera_send [pin]

    // 3. Read stdin in big blocks and find each JPEG in place: a frame starts
    //    with FF D8 and ends with FF D9. It is sent straight from this buffer
    //    (no copy) the moment its last byte arrives, so nothing waits.
    std::vector<uint8_t> buf(4 << 20);
    size_t have = 0, start = SIZE_MAX, scan = 0;
    unsigned sent = 0; size_t bytes = 0;
    auto lastPrint = std::chrono::steady_clock::now();

    for (;;) {
        if (have == buf.size()) {                     // frame bigger than the buffer: drop it
            have = 0; start = SIZE_MAX; scan = 0;
        }
        ssize_t n = read(0, buf.data() + have, buf.size() - have);
        if (n <= 0) break;
        have += (size_t)n;

        for (; scan + 1 < have; scan++) {
            if (buf[scan] != 0xFF) {                  // skip fast to the next marker
                const void* ff = memchr(buf.data() + scan + 1, 0xFF, have - scan - 1);
                if (!ff) { scan = have - 1; break; }
                scan = (const uint8_t*)ff - buf.data();
                if (scan + 1 >= have) break;
            }
            uint8_t m = buf[scan + 1];
            if (start == SIZE_MAX && m == 0xD8) start = scan;
            else if (start != SIZE_MAX && m == 0xD9) {
                // 4. A whole frame: send it if someone watches, else drop it
                size_t len = scan + 2 - start;
                if (r.virtualImageWrite(STREAM_PIN, buf.data() + start, len)) { sent++; bytes += len; }
                start = SIZE_MAX;
                scan++;
            }
        }
        // Keep only the unfinished frame (or the last byte) for the next read.
        size_t keep = start != SIZE_MAX ? start : (scan < have ? scan : have);
        memmove(buf.data(), buf.data() + keep, have - keep);
        have -= keep; scan -= keep;
        if (start != SIZE_MAX) start = 0;

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - lastPrint).count();
        if (dt >= 2) {
            printf("watching: %s  %.1f fps  %.1f KB/frame\n", r.virtualImageWanted() ? "yes" : "no ",
                   sent / dt, sent ? bytes / sent / 1024.0 : 0.0);
            fflush(stdout);
            sent = 0; bytes = 0; lastPrint = now;
        }
    }
}
