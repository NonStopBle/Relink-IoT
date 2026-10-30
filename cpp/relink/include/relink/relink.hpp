#pragma once
// Relink client for desktop/server C++ (Linux, macOS; needs OpenSSL).
//
// Same device token, same virtual pins as the ESP32 and Python libraries:
// every program connected with one token shares one set of pins. A write
// from any of them reaches the others and the dashboard -- peer-to-peer
// when they can reach each other directly (encrypted with the device key),
// through the relay otherwise.
//
//   relink::Client r(TOKEN, ENC_KEY, "192.168.96.50");
//   r.onPin(50, [](const relink::Value& v) { printf("V50=%f\n", v.asFloat()); });
//   r.start();                 // background thread (or call r.run() yourself)
//   r.virtualWrite(0, 21.5f);  // batched, sent on the next tick
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <netinet/in.h>

namespace relink {

// Relay copy per pin until the relay announces its rate (admin setting).
constexpr uint16_t RELAY_HZ_DEFAULT = 100;

enum class DType : uint8_t { Int16 = 0, Int32 = 1, Float = 2, String = 3, Bytes = 4 };   // Bytes: raw, in Value::s

struct Value {
    DType       type = DType::Int32;
    int32_t     i = 0;
    float       f = 0;
    std::string s;

    int32_t     asInt()    const { return type == DType::Float ? (int32_t)f : type == DType::String ? std::atoi(s.c_str()) : i; }
    float       asFloat()  const { return type == DType::Float ? f : type == DType::String ? (float)std::atof(s.c_str()) : (float)i; }
    std::string asString() const { return type == DType::String || type == DType::Bytes ? s : type == DType::Float ? std::to_string(f) : std::to_string(i); }
};

class Client {
public:
    using PinHandler = std::function<void(const Value&)>;
    using AnyHandler = std::function<void(uint8_t pin, const Value&)>;
    // jpeg points into the reassembly buffer and is valid only during the call.
    using CameraHandler = std::function<void(const uint8_t* jpeg, size_t len, uint16_t frameId)>;

    // encKeyHex: the device's 64-hex-char key, or "" for plaintext (the
    // device must then allow plaintext in the dashboard). Peer-to-peer mesh
    // needs encryption.
    Client(std::string token, std::string encKeyHex, std::string server,
           uint16_t port = 7445, bool mesh = true);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void onPin(uint8_t pin, PinHandler h);
    void onAny(AnyHandler h);
    // Live camera frames streamed on V<pin> (a Camera datastream) by a device
    // on this token (RelinkIoT virtualImageWrite).
    // Peer-to-peer only: needs a direct path to the device, never relayed.
    // Each slice is decrypted straight into its place in the frame buffer;
    // the handler gets the finished frame without any copy and runs on the
    // network thread, so keep it short (decode, or copy out and return).
    // Register before start().
    // Camera settings go to the device's settings pin as text, e.g.
    // virtualWrite(1, "fs=QVGA;q=20;fps=15").
    void onCamera(uint8_t pin, CameraHandler h);
    // The device streams to one viewer at a time -- whoever started watching
    // last. True once another viewer took it; watchCamera() takes it back.
    bool cameraKicked() const { return cam_kicked_; }
    void watchCamera() { cam_kicked_ = false; last_cam_sub_ = 0; }
    void setDebug(bool on) { debug_ = on; }

    void virtualWrite(uint8_t pin, int32_t v);
    void virtualWrite(uint8_t pin, float v);
    void virtualWrite(uint8_t pin, double v) { virtualWrite(pin, (float)v); }
    void virtualWrite(uint8_t pin, const std::string& v);
    void virtualWrite(uint8_t pin, const char* v) { virtualWrite(pin, std::string(v)); }
    void virtualWrite(uint8_t pin, const std::vector<uint8_t>& v);   // ByteArray datastream

    void flush();                 // send queued writes now
    void run();                   // one non-blocking step (if not using start())
    void start();                 // run() in a background thread
    void stop();

    bool connected() const;
    // instance id -> true when reached peer-to-peer, false when via relay
    std::map<uint16_t, bool> peers();
    uint16_t instance() const { return instance_; }

private:
    struct Peer {
        uint16_t instance = 0;
        std::vector<sockaddr_in> cands;
        sockaddr_in addr{};
        bool has_addr = false;
        double last_rx = 0, last_hello = 0;
        double since = -1;      // first listed by the relay (hello priming window)
        bool gateway = false;   // its hello says a P2P browser gets our writes through it
        bool up(double now) const { return has_addr && now - last_rx < 5.0; }
    };

    void queue(std::string entry);
    void sendEnc(const std::string& inner, const sockaddr_in& to, int ttl = 0);
    void sendRaw(const std::string& b, const sockaddr_in& to, int ttl = 0);
    void doRegister();
    void sendLinks(bool force);
    void addSummary(uint8_t pin, float v);
    void sendSummary();
    void onServer(const uint8_t* b, size_t n);
    void onPeer(const uint8_t* b, size_t n, const sockaddr_in& from);
    uint8_t* camSlot(const uint8_t* head, size_t len);
    void camCommit(const uint8_t* head, size_t len);
    void dispatch(uint8_t pin, uint8_t dtype, const uint8_t* v, size_t n, bool len_prefixed = true);
    void refreshPins();
    uint64_t nextNonce();

    std::string token_, server_host_;
    sockaddr_in server_{};
    uint32_t device_id_ = 0;
    uint8_t  key_[32]{};
    bool      api_synced_ = false;   // server clock offset known (signed API calls)
    long long api_offset_ = 0;
    bool     secure_ = false, mesh_ = true, debug_ = false;
    uint16_t instance_ = 0;
    uint64_t relay_nonce_ = 0;   // last sealed downlink taken from the relay
    std::atomic<uint32_t> salt_{0};   // relay session salt ('Y'), 0 = none yet (S6)
    uint64_t ctr_ = 0;
    int      fd_ = -1;
    std::string lan_;

    std::mutex mu_;                      // tx_, handlers_, peers_, allowed_
    std::vector<std::string> tx_;
    std::map<uint8_t, std::vector<PinHandler>> handlers_;
    std::vector<AnyHandler> any_;
    std::map<uint8_t, std::vector<CameraHandler>> cam_handlers_;   // stream pin -> handlers
    struct CamFrame {                    // one pin's reassembly, network thread only
        std::vector<uint8_t> buf, seen;
        int fid = -1;
        uint16_t cnt = 0, got = 0;
        size_t size = 0;
    };
    std::map<uint8_t, CamFrame> cams_;
    double last_cam_sub_ = 0;
    std::atomic<bool> cam_kicked_{false};
    std::map<uint16_t, Peer> peers_;
    // Replay window per peer (as in DTLS/IPsec): each nonce once, up to
    // REPLAY_WINDOW behind the highest seen -- a device sends camera slices
    // from two cores, so its packets interleave.
    static constexpr uint64_t REPLAY_WINDOW = 1024;
    struct ReplayWin { uint64_t top = 0; uint64_t bits[REPLAY_WINDOW / 64] = {}; };
    std::map<uint16_t, ReplayWin> peer_rx_;
    bool replaySeen(uint16_t inst, uint64_t nonce);   // under mu_
    void replayMark(uint16_t inst, uint64_t nonce);   // under mu_
    std::vector<bool> allowed_;          // empty = not fetched yet
    std::vector<uint16_t> last_links_;
    struct PinSum { uint32_t n = 0; float mn = 0, mx = 0, sum = 0, last = 0; };
    uint16_t relay_hz_ = RELAY_HZ_DEFAULT; // from the relay's peer list, under mu_ (0 = no limit)
    bool relay_off_ = false;               // same list: no relay copy while all peers are direct
    std::map<uint8_t, std::string> relay_tx_;   // latest entry per pin for the relay, under mu_
    std::map<uint8_t, PinSum> sum_;      // 1-minute history summary ('M'), under mu_

    std::atomic<bool> connected_{false};
    std::atomic<double> last_rx_server_{0};
    double last_reg_ = -10, last_links_sent_ = 0, last_pins_ = -100, last_summary_ = 0, last_relay_tx_ = 0;

    std::thread thread_;
    std::atomic<bool> stop_{false};
};

} // namespace relink
