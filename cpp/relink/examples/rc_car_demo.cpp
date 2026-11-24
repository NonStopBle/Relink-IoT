// RC car demo -- one program plays a whole RC car for the dashboard: live video,
// GPS on the map, battery, speed and serial-monitor lines. The joystick drives the
// simulated car: push forward and the speed climbs, the map dot moves, the battery
// drains faster. Used to record the app demo video; no real car needed.
//
// Dashboard setup:  V5 Camera, V4 String (serial monitor), V6 Double lat, V7 Double lng,
//                   V2 battery, V3 speed, V0/V1 joystick X/Y (override with PIN_* env vars)
//                   V8 Audio (the clip's sound at 48 kHz; each listener gets the quality it picks)
// Needs:            an encryption key (video and audio are P2P), ffmpeg on PATH
// Run:              ./relink_rc_car_demo clip.mp4 [more.mp4 ...]   (plays the clips in order, then loops;
//                   several clips must share codec, size and audio format)
#include <relink/relink.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include "settings.hpp"   // 1. Your settings live here

static int pinEnv(const char* name, int def) { const char* v = getenv(name); return v && *v ? atoi(v) : def; }

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s clip.mp4 [more.mp4 ...]\n", argv[0]); return 1; }
    // One clip: loop it. Several: an ffmpeg concat playlist, so picture and sound
    // (two ffmpeg processes) walk the same clips in the same order.
    std::string input;
    if (argc == 2) {
        input = std::string("-i '") + argv[1] + "'";
    } else {
        char list[] = "/tmp/relink_rc_car_XXXXXX";
        int lfd = mkstemp(list);
        if (lfd < 0) { perror("mkstemp"); return 1; }
        FILE* lf = fdopen(lfd, "w");
        for (int i = 1; i < argc; i++) {
            char* abs = realpath(argv[i], nullptr);
            if (!abs) { perror(argv[i]); return 1; }
            fprintf(lf, "file '%s'\n", abs);
            free(abs);
        }
        fclose(lf);
        input = std::string("-f concat -safe 0 -i '") + list + "'";
    }
    const uint8_t CAM = pinEnv("PIN_CAM", 5), SERIAL = pinEnv("PIN_SERIAL", 4),
                  LAT = pinEnv("PIN_LAT", 6), LNG = pinEnv("PIN_LNG", 7),
                  BATT = pinEnv("PIN_BATT", 2), SPEED = pinEnv("PIN_SPEED", 3),
                  JOY = pinEnv("PIN_JOY", 0), AUDIO = pinEnv("PIN_AUDIO", 8);

    // 2. Connect
    relink::Client r(RELINK_TOKEN_, RELINK_KEY_, RELINK_SERVER_);
    if (getenv("RELINK_DEBUG")) r.setDebug(true);   // P2P / audio-port log on stderr
    r.enableP2P();

    // 3. Joystick: accept "x,y" strings or a single number; log every write so the
    //    pin and format show up in the terminal.
    std::atomic<float> joyX{0}, joyY{0};
    r.onAny([&](uint8_t pin, const relink::Value& v) {
        std::string s = v.asString();
        // The joystick widget writes X on JOY and Y on JOY+1, 0..100 with 50 = center.
        if (pin == JOY)     joyX = ((float)atof(s.c_str()) - 50.f) / 50.f;
        if (pin == JOY + 1) joyY = ((float)atof(s.c_str()) - 50.f) / 50.f;
    });
    r.start();

    // 4. Telemetry loop in its own thread: 10 Hz physics, values at 5 Hz, a serial line each second.
    std::thread tele([&] {
        // A loop of park paths around the lake in Lumphini Park, Bangkok. The car follows
        // it like a real track (no free steering, so the dot never spins in place); the
        // joystick only sets the speed.
        static const double route[][2] = {
            {13.73220, 100.53900}, {13.73270, 100.54100}, {13.73230, 100.54320}, {13.73120, 100.54450},
            {13.72970, 100.54460}, {13.72830, 100.54380}, {13.72770, 100.54200}, {13.72810, 100.53980},
            {13.72950, 100.53860}, {13.73100, 100.53840},
        };
        const int N = sizeof route / sizeof route[0];
        const double kLng = cos(13.73 * M_PI / 180);
        int seg = 0; double along = 0;                                   // metres into segment seg
        double lat = route[0][0], lng = route[0][1], heading = 0;
        double speed = 0, batt = 92.0;
        int tick = 0;
        char line[64];
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));   // 100 Hz telemetry
            float y = joyY;
            double target = std::max(0.0, (double)y) * 38.0;          // km/h at full stick
            // RC_AUTO=1: with the stick idle the car cruises by itself (demo videos).
            static const bool autoDrive = getenv("RC_AUTO") != nullptr;
            if (autoDrive && fabs(y) < 0.05)
                target = 22.0 + 12.0 * sin(tick * 0.002) + 4.0 * sin(tick * 0.0077);
            speed += (target - speed) * 0.015;
            along += speed / 3.6 * 0.01 * 6;                             // map moves 6x faster so it shows
            for (;;) {
                const double* a = route[seg]; const double* b = route[(seg + 1) % N];
                double dn = (b[0] - a[0]) * 111320.0, de = (b[1] - a[1]) * 111320.0 * kLng;
                double len = sqrt(dn * dn + de * de);
                if (along < len) {
                    double t = along / len, want = atan2(de, dn);
                    double d = remainder(want - heading, 2 * M_PI);
                    heading += d * 0.025;                                // round the corners
                    lat = a[0] + (b[0] - a[0]) * t;
                    lng = a[1] + (b[1] - a[1]) * t;
                    break;
                }
                along -= len; seg = (seg + 1) % N;
            }
            // A real GPS fix wobbles a metre or two.
            double jl = (rand() / (double)RAND_MAX - 0.5) * 0.5e-5, jg = (rand() / (double)RAND_MAX - 0.5) * 0.5e-5;
            lat += jl; lng += jg;
            batt -= 0.00008 + speed * 0.000012;
            if (batt < 15) batt = 92;
            if (++tick) {
                r.virtualWrite(LAT, lat);
                r.virtualWrite(LNG, lng);
                r.virtualWrite(SPEED, (int32_t)lround(speed));
                r.virtualWrite(BATT, (int32_t)lround(batt));
            }
            if (tick % 20 == 0) {                                     // serial line 5 Hz
                snprintf(line, sizeof line, "spd %2.0f km/h  bat %2.0f%%  hdg %3.0f", speed, batt,
                         fmod(heading * 180 / M_PI + 360, 360));
                r.virtualWrite(SERIAL, line);
            }
        }
    });
    tele.detach();

    // 5. Sound: the clip's audio as 48 kHz mono PCM, paced by ffmpeg -re, 10 ms at a time.
    //    The library resamples it to each listener's quality (Voice / Standard / High).
    std::thread aud([&] {
        std::string cmd = "ffmpeg -loglevel error -re -stream_loop -1 " + input +
                          " -vn -ac 1 -ar 48000 -f s16le -";
        FILE* a = popen(cmd.c_str(), "r");
        if (!a) return;
        int16_t pcm[480];
        while (fread(pcm, 2, 480, a) == 480) r.audioWrite(AUDIO, pcm, 480, 48000);
        pclose(a);
    });
    aud.detach();

    // 6. Video: MJPEG from the clip, same in-place frame finder as camera_send.cpp.
    std::string vcmd = "ffmpeg -loglevel error -re -stream_loop -1 " + input +
                       " -an -vf scale=960:-2 -r 24 -q:v 5 -f mjpeg -";
    FILE* vf = popen(vcmd.c_str(), "r");
    if (!vf) { perror("ffmpeg"); return 1; }
    const int vfd = fileno(vf);
    std::vector<uint8_t> buf(4 << 20);
    size_t have = 0, start = SIZE_MAX, scan = 0;
    unsigned sent = 0;
    auto lastPrint = std::chrono::steady_clock::now();
    for (;;) {
        if (have == buf.size()) { have = 0; start = SIZE_MAX; scan = 0; }
        ssize_t n = read(vfd, buf.data() + have, buf.size() - have);
        if (n <= 0) break;
        have += (size_t)n;
        for (; scan + 1 < have; scan++) {
            if (buf[scan] != 0xFF) {
                const void* ff = memchr(buf.data() + scan + 1, 0xFF, have - scan - 1);
                if (!ff) { scan = have - 1; break; }
                scan = (const uint8_t*)ff - buf.data();
                if (scan + 1 >= have) break;
            }
            uint8_t mk = buf[scan + 1];
            if (start == SIZE_MAX && mk == 0xD8) start = scan;
            else if (start != SIZE_MAX && mk == 0xD9) {
                if (r.virtualImageWrite(CAM, buf.data() + start, scan + 2 - start)) sent++;
                start = SIZE_MAX; scan++;
            }
        }
        size_t keep = start != SIZE_MAX ? start : (scan < have ? scan : have);
        memmove(buf.data(), buf.data() + keep, have - keep);
        have -= keep; scan -= keep;
        if (start != SIZE_MAX) start = 0;
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - lastPrint).count();
        if (dt >= 2) {
            printf("video: %s %.1f fps  joy %.2f,%.2f\n", r.virtualImageWanted() ? "watched" : "idle   ",
                   sent / dt, (float)joyX, (float)joyY);
            fflush(stdout);
            sent = 0; lastPrint = now;
        }
    }
}
