// Camera + mic -- live video and two-way sound between programs, peer-to-peer.
//
// One program, two roles on the same token:
//   viewer (default)  receives the camera (newest frame saved with --jpg),
//                     plays the sound on the PC speakers and sends the PC
//                     microphone back (talk)
//   --send            acts like the camera board: streams MJPEG from stdin and
//                     the PC microphone, and plays what the viewer says
//
// Dashboard setup:  V10 Camera, V11 ByteArray, V0 Audio
// Pairs with:       itself (--send on one PC, viewer on another), or the Arduino
//                   "CameraStreamWithMic" example (run the viewer)
// Needs:            an encryption key (P2P only); PortAudio (libportaudio2 +
//                   portaudio19-dev) for the speaker and mic; ffmpeg for --send
// Run:              viewer:
//                     ./relink_camera_mic --jpg latest.jpg
//                   device, webcam + mic:
//                     ffmpeg -loglevel error -f v4l2 -i /dev/video0 -vf scale=640:-2 \
//                       -r 15 -q:v 5 -f mjpeg - | ./relink_camera_mic --send
//                   other options: --mute (listen only), --tone 440 (send a tone,
//                   not the mic), --seconds 20, --save heard.wav
// You'll see:       "video 15.0 fps | heard 50 pkt/s 52 dB | said 50 pkt/s | ..."
//                   every 2 s. Use headphones: a speaker next to the mic echoes.
#include <relink/relink.hpp>
#include <portaudio.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include "settings.hpp"   // 1. Your settings live here (needs a KEY: P2P mesh)

static const int RATE = 8000, PKT = 160;   // 8 kHz mono, 20 ms per packet
static const uint8_t STREAM_PIN = 10, AUDIO_PIN = 0;

// Packets waiting for the speaker / for the network. The sound card's
// callbacks only touch these under a lock, never the network.
struct Queue {
    std::mutex mu;
    std::deque<std::vector<int16_t>> q;
    void push(const int16_t* p, size_t n) {
        std::lock_guard<std::mutex> lk(mu);
        q.emplace_back(p, p + n);
        if (q.size() > 25) q.pop_front();     // keep at most 0.5 s
    }
    bool pop(std::vector<int16_t>& out) {
        std::lock_guard<std::mutex> lk(mu);
        if (q.empty()) return false;
        out = std::move(q.front()); q.pop_front();
        return true;
    }
    size_t size() { std::lock_guard<std::mutex> lk(mu); return q.size(); }
};
static Queue play, mic;

// Speaker: wait until 3 packets are queued before starting (absorbs Wi-Fi
// jitter); a gap plays as silence.
static int speaker(const void*, void* out, unsigned long frames, const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void*) {
    static bool primed = false;
    static std::vector<int16_t> pkt;
    int16_t* o = (int16_t*)out;
    if (!primed && play.size() >= 3) primed = true;
    if (primed && play.pop(pkt) && pkt.size() == frames) memcpy(o, pkt.data(), frames * 2);
    else { memset(o, 0, frames * 2); primed = primed && play.size() > 0; }
    return paContinue;
}
static int microphone(const void* in, void*, unsigned long frames, const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void*) {
    if (in) mic.push((const int16_t*)in, frames);
    return paContinue;
}

static void writeWav(const char* path, const std::vector<int16_t>& s) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t data = (uint32_t)s.size() * 2, riff = 36 + data, fmt = 16, rate = RATE, bps = RATE * 2;
    uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&bps, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(s.data(), 2, s.size(), f);
    fclose(f);
}

int main(int argc, char** argv) {
    auto has = [&](const char* k) { for (int i = 1; i < argc; i++) if (!strcmp(argv[i], k)) return true; return false; };
    auto arg = [&](const char* k, const char* d) { for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], k)) return (const char*)argv[i + 1]; return d; };
    const bool send = has("--send"), mute = has("--mute");
    const double tone = atof(arg("--tone", "0")), seconds = atof(arg("--seconds", "0"));
    const char* save = arg("--save", nullptr);
    const char* jpg = arg("--jpg", nullptr);

    // 2. Connect
    relink::Client r(RELINK_TOKEN_, RELINK_KEY_, RELINK_SERVER_);
    if (send) r.enableP2P();   // lets the dashboard widgets watch too

    // 3. Handlers: frames (viewer) and sound (both)
    std::atomic<long> frames{0}, heard{0}, said{0};
    std::mutex wavMu;
    std::vector<int16_t> wav;
    double sq = 0;
    if (!send)
        r.onCamera(STREAM_PIN, [&](const uint8_t* jpeg, size_t len, uint16_t) {
            frames++;
            if (jpg) if (FILE* f = fopen(jpg, "wb")) { fwrite(jpeg, 1, len, f); fclose(f); }
        });
    r.onAudio(AUDIO_PIN, [&](const int16_t* pcm, size_t n) {
        play.push(pcm, n);
        heard++;
        std::lock_guard<std::mutex> lk(wavMu);
        for (size_t i = 0; i < n; i++) sq += (double)pcm[i] * pcm[i];
        if (save) wav.insert(wav.end(), pcm, pcm + n);
    });
    r.start();

    auto direct = [&] { for (auto& [id, up] : r.peers()) if (up) return true; return false; };   // sound is P2P only

    // Sound card: speaker always, microphone unless --mute / --tone
    if (Pa_Initialize() != paNoError) { fprintf(stderr, "PortAudio: no sound system\n"); return 1; }
    PaStream *out = nullptr, *in = nullptr;
    if (Pa_OpenDefaultStream(&out, 0, 1, paInt16, RATE, PKT, speaker, nullptr) == paNoError) Pa_StartStream(out);
    else fprintf(stderr, "no speaker\n");
    if (!mute && !tone) {
        if (Pa_OpenDefaultStream(&in, 1, 0, paInt16, RATE, PKT, microphone, nullptr) == paNoError) Pa_StartStream(in);
        else fprintf(stderr, "no microphone\n");
    }

    // Talk thread: sends the mic blocks (or a tone, one packet per 20 ms)
    std::atomic<bool> running{true};
    auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    std::thread talk([&] {
        long phase = 0;
        std::vector<int16_t> block(PKT);
        while (running) {
            if (tone && !mute) {
                for (; phase < elapsed() * RATE; phase += PKT) {
                    for (int i = 0; i < PKT; i++) block[i] = (int16_t)(9000 * sin(2 * M_PI * tone * (phase + i) / RATE));
                    if (direct()) said += r.audioWrite(AUDIO_PIN, block.data(), PKT);
                }
            }
            while (mic.pop(block))
                if (direct()) said += r.audioWrite(AUDIO_PIN, block.data(), block.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // Stats every 2 s
    std::thread stats([&] {
        while (running) {
            for (int i = 0; i < 20 && running; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            long h = heard.exchange(0);
            double level;
            { std::lock_guard<std::mutex> lk(wavMu); level = h ? 20 * log10(sqrt(sq / (h * PKT)) + 1) : 0; sq = 0; }
            printf("video %4.1f fps | heard %3ld pkt/s %3.0f dB | said %3ld pkt/s | ",
                   frames.exchange(0) / 2.0, h / 2, level, said.exchange(0) / 2);
            printStatus(r);
            if (seconds && elapsed() >= seconds) running = false;
        }
    });

    // 4. Loop. --send: read MJPEG from stdin, cut it at FF D8 .. FF D9 and send
    //    each frame while someone watches. Viewer: just wait.
    if (send) {
        std::vector<uint8_t> buf(4 << 20);
        size_t have = 0;
        while (running) {
            ssize_t n = read(0, buf.data() + have, buf.size() - have);
            if (n <= 0) break;
            have += (size_t)n;
            size_t start = SIZE_MAX, used = 0;
            for (size_t i = 0; i + 1 < have; i++) {
                if (buf[i] != 0xFF) continue;
                if (buf[i + 1] == 0xD8 && start == SIZE_MAX) start = i;
                else if (buf[i + 1] == 0xD9 && start != SIZE_MAX) {
                    if (r.virtualImageWrite(STREAM_PIN, buf.data() + start, i + 2 - start)) frames++;
                    start = SIZE_MAX; used = i + 2;
                }
            }
            memmove(buf.data(), buf.data() + used, have - used);
            have -= used;
            if (have == buf.size()) have = 0;     // frame bigger than the buffer: drop it
        }
        if (running) { fprintf(stderr, "stdin closed\n"); running = false; }
    }
    while (running) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    talk.join(); stats.join();
    if (in) Pa_CloseStream(in);
    if (out) Pa_CloseStream(out);
    Pa_Terminate();
    r.stop();
    if (save) { writeWav(save, wav); printf("%s: %.1f s\n", save, wav.size() / (double)RATE); }
    return 0;
}
