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
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#endif

namespace relink {

// Relay copy per pin until the relay announces its rate (admin setting).
constexpr uint16_t RELAY_HZ_DEFAULT = 100;

// Bytes: raw, in Value::s. UInt8: 0..255, in Value::i. Double (Float64): in Value::d.
enum class DType : uint8_t { Int16 = 0, Int32 = 1, Float = 2, String = 3, Bytes = 4, UInt8 = 5, Double = 6 };

// One integer pin value read as a number or as bits; the sketch keeps it, the
// library stores nothing per bit. Members share the same 4 bytes (u8/i16 are
// the low bytes on little-endian hosts). Send with virtualWrite(pin, bits).
union Bits {
    uint8_t  u8;
    int16_t  i16;
    int32_t  i32;
    uint32_t raw;
    Bits(int32_t v = 0) : i32(v) {}
    bool asBit(unsigned n) const { return n < 32 && ((raw >> n) & 1u); }
    void setBit(unsigned n, bool on) { if (n < 32) raw = on ? (raw | (1u << n)) : (raw & ~(1u << n)); }
};

struct Value {
    DType       type = DType::Int32;
    int32_t     i = 0;
    float       f = 0;
    double      d = 0;
    std::string s;

    double      asDouble() const { return type == DType::Float ? f : type == DType::Double ? d
                                        : type == DType::String ? std::atof(s.c_str()) : (double)i; }
    int32_t     asInt()    const { return type == DType::String ? std::atoi(s.c_str()) : (int32_t)asDouble(); }
    uint8_t     asUInt8()  const { return (uint8_t)asInt(); }
    float       asFloat()  const { return (float)asDouble(); }
    // One bit (0 = LSB) of an integer value (UInt8 / Integer datastreams).
    bool        asBit(unsigned n)  const { return n < 32 && (((uint32_t)asInt() >> n) & 1u); }
    bool        getBit(unsigned n) const { return asBit(n); }
    operator Bits() const { return Bits(asInt()); }
    std::string asString() const { return type == DType::String || type == DType::Bytes ? s : type == DType::Float ? std::to_string(f)
                                        : type == DType::Double ? std::to_string(d) : std::to_string(i); }
};

class Client {
public:
    using PinHandler = std::function<void(const Value&)>;
    using AnyHandler = std::function<void(uint8_t pin, const Value&)>;
    // jpeg points into the reassembly buffer and is valid only during the call.
    using CameraHandler = std::function<void(const uint8_t* jpeg, size_t len, uint16_t frameId)>;
    using AudioHandler = std::function<void(const int16_t* pcm, size_t n)>;
    // Same, plus the sample rate of this packet (8000, 16000 or 48000).
    using AudioRateHandler = std::function<void(const int16_t* pcm, size_t n, int rate)>;
    // Listen quality, asked of the sender in each audio subscribe.
    enum AudioQuality : uint8_t { AUDIO_VOICE = 0, AUDIO_STANDARD = 1, AUDIO_HIGH = 2 };
    static int audioQualityRate(int q) { return q >= AUDIO_HIGH ? 48000 : q == AUDIO_STANDARD ? 16000 : 8000; }

    // encKeyHex: the device's 64-hex-char key, or "" for plaintext (the
    // device must then allow plaintext in the dashboard). Peer-to-peer mesh
    // needs encryption.
    Client(std::string token, std::string encKeyHex, std::string server,
           uint16_t port = 7445, bool mesh = true);
    ~Client();
    Client(const Client&) = delete;
    // Viewer app (phone/dashboard), not the device: registers with caps 'A' so the
    // relay doesn't count it as the device online or list it to other apps. Before start().
    void setApp(bool on) { app_ = on; }
    // Relay ports tried in turn when the relay stops answering (UDP 7445 blocked
    // on some networks). Default: the given port, then 443 (the server redirects
    // UDP 443 to the relay). Before start().
    void setRelayPorts(std::vector<uint16_t> ports) { if (!ports.empty()) { relay_ports_ = std::move(ports); port_idx_ = 0; server_.sin_port = htons(relay_ports_[0]); } }
    uint16_t relayPort() const { return ntohs(server_.sin_port); }
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
    // Pause / resume watching (default on). Off sends 'stop' to the device
    // once, so the stream ends now instead of after its 3 s timeout -- for a
    // viewer that registers onCamera() up front but shows the picture only
    // some of the time (the Android app's Camera widget).
    void setCameraWatch(bool on) { if (on && !cam_watch_) watchCamera(); cam_watch_ = on; }

    // Sending video, like an ESP32-CAM: one JPEG frame on V<pin> (a Camera
    // datastream) to the viewer that is watching, peer-to-peer over the
    // encrypted mesh, never through the relay. The viewer is another C++ /
    // Python program using onCamera(), or the dashboard's Live Camera widget
    // over its data channel (needs enableP2P()). The newest viewer takes the
    // stream. False = nothing sent (nobody watching, or no encryption key).
    // Check virtualImageWanted() before grabbing and encoding a frame.
    // Live audio, 8 kHz mono G.711 µ-law in 20 ms packets, peer-to-peer only.
    // onAudio: sound a device streams on V<pin> (an Audio datastream, RelinkIoT
    // audioWrite) -- 160 samples per call on the network thread; while a
    // handler is set this program asks the device for the stream every second.
    // Register before start().
    void onAudio(uint8_t pin, AudioHandler h);
    // Rate-aware handler: needed for Standard / High quality (16 / 48 kHz PCM,
    // 'w' packets). A plain onAudio handler only ever gets 8 kHz.
    void onAudioRate(uint8_t pin, AudioRateHandler h);
    // Quality this program asks the sender for (default Voice). A sender that
    // doesn't know the levels keeps sending Voice; the handler's rate says what came.
    void setAudioQuality(int q) { aud_quality_ = (uint8_t)(q < 0 ? 0 : q > 2 ? 2 : q); last_aud_sub_ = 0; }
    // audioWrite: talk to the devices on this token (their sketch plays it
    // with Relink.onAudio). Samples are sent in 20 ms packets to every direct
    // peer; a partial packet waits for the next call. Returns packets sent.
    int audioWrite(uint8_t pin, const int16_t* pcm, size_t n);
    // Same, from mono PCM at any sampleRate (e.g. 48000). Each listener gets the
    // quality it asked for: Voice ('A', 8 kHz µ-law), Standard (16 kHz PCM) or
    // High (48 kHz PCM); the library resamples once per quality in use.
    int audioWrite(uint8_t pin, const int16_t* pcm, size_t n, int sampleRate);
    // Pause / resume listening (default on). Off tells the device once, so it
    // stops reading its mic now instead of after its 3 s timeout.
    void setAudioListen(bool on) { aud_listen_ = on; if (on) last_aud_sub_ = 0; }

    bool virtualImageWanted();
    bool virtualImageWrite(uint8_t pin, const uint8_t* jpeg, size_t len);
    void setDebug(bool on) { debug_ = on; }

    // Dashboard P2P: answer the dashboard's WebRTC offer, so its widgets get
    // this program's pins over a direct data channel (and its writes come
    // straight here), like an ESP32 with the RelinkIoT library. The relay
    // path keeps working alongside; nothing is lost when P2P can't connect.
    // udpPort 0 = any free port. Needs the encryption key (signed
    // signaling) and a library built with mbedTLS -- false otherwise.
    // Only one program per token should call this: the first to answer an
    // offer gets the dashboard.
    bool enableP2P(uint16_t udpPort = 0);
    bool p2pConnected();

    void virtualWrite(uint8_t pin, int32_t v);
    void virtualWrite(uint8_t pin, float v);
    // A Float64 datastream keeps all 8 bytes; on any other number pin it converts.
    void virtualWrite(uint8_t pin, double v);
    void virtualWrite(uint8_t pin, uint8_t v);   // UInt8 datastream
    // Integer / UInt8 pin as bits (see Bits): sends the whole value.
    void virtualWrite(uint8_t pin, const Bits& b) { virtualWrite(pin, b.i32); }
    void virtualWrite(uint8_t pin, const std::string& v);
    void virtualWrite(uint8_t pin, const char* v) { virtualWrite(pin, std::string(v)); }
    void virtualWrite(uint8_t pin, const std::vector<uint8_t>& v);   // ByteArray datastream

    void flush();                 // send queued writes now
    void run();                   // one non-blocking step (if not using start())
    void start();                 // run() in a background thread
    // Low power (e.g. a phone app in the background): start()'s thread wakes on
    // packets or every 500 ms instead of every 2 ms, no mesh hellos (pins come
    // through the relay), pin list refreshed every 5 min. Writes still go out
    // at once if you call flush(). Off by default; can change while running.
    void setLowPower(bool on);
    // Mesh priming: the first hellos go out with TTL 2 so they open our own NAT
    // but die before the peer's. Right behind a home router; wrong behind a
    // carrier NAT several hops away (cellular), where they never reach it.
    void setMeshPriming(bool on) { priming_ = on; }
    void stop();

    bool connected() const;
    // instance id -> true when reached peer-to-peer, false when via relay
    std::map<uint16_t, bool> peers();
    // Pin values received so far: straight from a peer, and through the relay.
    // Diagnostics (tests, debug builds) -- shows which path the data took.
    void rxStats(uint64_t& direct, uint64_t& relay) const { direct = rx_direct_; relay = rx_relay_; }
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
        double cam_rx = 0;      // its last camera subscribe ('C'), when we send video
        uint8_t aud_q = 0;      // audio quality it asked for in its last 'a' subscribe
        // Its audio socket (a second UDP port, audio only, so sound never queues
        // behind a camera burst). Learned from its 'h' probes; up while its audio
        // keeps arriving on our audio socket.
        sockaddr_in aud_addr{};
        bool has_aud = false;
        bool aud_cap = false;   // its hellos carry flag bit 1: it has an audio port (C++ lib / app)
        double aud_rx = 0;
        bool audUp(double now) const { return has_aud && now - aud_rx < 5.0; }
        // heard: its packets reach us. up: a working link both ways -- its
        // hellos say it hears us too (flag bit2). On mobile data a phone often
        // hears a board that can't hear it; treating that as direct lost every
        // write both ways. Old libraries send no confirmation: heard = up.
        bool heard(double now) const { return has_addr && now - last_rx < 5.0; }
        bool legacy = true;     // its hellos have no address/confirmation (pre-1.0.9)
        double conf_at = -100;  // last hello saying it hears us
        double last_ping = -100;  // our last 'F' keep-alive to it (it can't hear us)
        double last_diag = -100;  // debug: last [diag] line for it
        bool up(double now) const { return heard(now) && (legacy || now - conf_at < 5.0); }
        uint8_t reach[6] = {};   // [ip:4][port:2 BE] its hellos were sent to (reaches us)
        double reach_at = -100;
        // Its request came through the relay ('F'): we can't hear it directly
        // (mobile data gives it a new port per destination), but it hears us
        // at fwd_addr (from its 'F') or at the address the relay sees (cands[0]).
        double fwd_rx = -100;
        sockaddr_in fwd_addr{};
        bool has_fwd = false;
        bool reachable(double now) const { return up(now) || (now - fwd_rx < 5.0 && (has_fwd || !cands.empty())); }
        sockaddr_in tx(double now) const { return up(now) ? addr : has_fwd ? fwd_addr : cands.empty() ? addr : cands[0]; }
    };

    void queue(std::string entry);
    // via: the socket to send from (-1 = the main one, fd_).
    void sendEnc(const std::string& inner, const sockaddr_in& to, int ttl = 0, int via = -1);
    void sendHello(const sockaddr_in& to, int ttl = 0, bool hear = false);
    void peerCtl(uint16_t inst, Peer& p, const uint8_t* m, size_t ml, double now);
    void sendRaw(const std::string& b, const sockaddr_in& to, int ttl = 0, int via = -1);
    void doRegister();
    void sendLinks(bool force);
    void addSummary(uint8_t pin, float v);
    void sendSummary();
    void onServer(const uint8_t* b, size_t n);
    // Our public addresses as several STUN servers see the main socket ('T' to
    // the relay): networks with several public IPs pick one per destination.
    void stunKick();
    void onStun(const uint8_t* b, size_t n);
    void sendPublic();
    void onPeer(const uint8_t* b, size_t n, const sockaddr_in& from, bool audioSock = false);
    uint8_t* camSlot(const uint8_t* head, size_t len);
    void camCommit(const uint8_t* head, size_t len);
    void dispatch(uint8_t pin, uint8_t dtype, const uint8_t* v, size_t n, bool len_prefixed = true);
    void refreshPins();
    void parsePinsJson(const std::string& body);
    std::string apiAuth(const char* method, const std::string& path, const std::string& body);
    bool apiSync();
    // Dashboard P2P (relink.cpp; empty without mbedTLS)
    struct Rtc;
    std::unique_ptr<Rtc> rtc_;
    void rtcPoll();
    void rtcSignal();             // signaling thread: fetch an offer, answer it
    void rtcSendPins(const std::vector<std::string>& entries);
    static void rtcOnMessage(void* user, const uint8_t* data, size_t len);
    uint64_t nextNonce();

    std::string token_, server_host_;
    sockaddr_in server_{};
    uint32_t device_id_ = 0;
    uint8_t  key_[32]{};
    bool      api_synced_ = false;   // server clock offset known (signed API calls)
    long long api_offset_ = 0;
    bool     p2p_off_ = false;   // auto-enable of P2P already tried
    bool     secure_ = false, mesh_ = true, debug_ = false, app_ = false;
    double   last_rxlog_ = 0;   // debug: last [diag] rx line
    std::atomic<bool> low_power_{false};
    std::atomic<bool> priming_{true};
    uint16_t instance_ = 0;
    double   unknown_peer_reg_ = 0;   // last re-register asked for by an unlisted peer
    uint64_t relay_nonce_ = 0;   // last sealed downlink taken from the relay
    std::atomic<uint32_t> salt_{0};   // relay session salt ('Y'), 0 = none yet (S6)
    uint64_t ctr_ = 0;
    int      fd_ = -1;
    std::string lan_;

    std::mutex mu_;                      // tx_, handlers_, peers_, allowed_
    std::vector<std::string> tx_;
    std::vector<std::string> tx_nodc_;   // writes from the P2P browser: to mesh / relay only
    std::map<uint8_t, std::vector<PinHandler>> handlers_;
    std::vector<AnyHandler> any_;
    std::map<uint8_t, std::vector<CameraHandler>> cam_handlers_;   // stream pin -> handlers
    std::map<uint8_t, std::vector<AudioRateHandler>> aud_handlers_; // audio pin -> handlers
    std::map<uint8_t, std::vector<int16_t>> aud_buf_;              // 8 kHz samples waiting for a full packet (audioWrite)
    // audioWrite(..., sampleRate): one resampler + packet buffer per (pin, quality).
    struct AudOut { int in_rate = 0; double pos = 0; std::vector<int16_t> hist, pend; std::vector<float> taps; };
    std::map<std::pair<uint8_t, uint8_t>, AudOut> aud_out_;
    std::atomic<uint8_t> aud_quality_{0};
    // Where a peer's audio goes: its audio socket from ours when that path is up,
    // else the main link (an ESP32, or a NAT that blocked the second port).
    struct AudDest { sockaddr_in addr; int via; };
    void audSend(uint8_t pin, uint8_t q, const std::vector<AudDest>& to, AudOut& o, int& sent);
    int afd_ = -1;   // audio-only UDP socket, peer-to-peer only (never the relay)
    double last_aud_sub_ = 0;
    std::atomic<bool> aud_listen_{true};
    bool aud_stop_sent_ = false;
    uint16_t aud_sid_ = 0, aud_seq_ = 0;
    struct CamFrame {                    // one pin's reassembly, network thread only
        std::vector<uint8_t> buf, seen;
        int fid = -1;
        uint16_t cnt = 0, got = 0;
        size_t size = 0;
    };
    std::map<uint8_t, CamFrame> cams_;
    double last_cam_sub_ = 0;
    std::atomic<bool> cam_kicked_{false};
    std::atomic<bool> cam_watch_{true};
    bool cam_stop_sent_ = true;          // network thread
    int cam_owner_ = -1;                 // sender side: instance we stream to, CAM_BROWSER, or -1 (under mu_)
    static constexpr int CAM_BROWSER = -2;   // the dashboard over P2P
    bool cam_browser_ = false;           // dashboard's widget is on screen ([0x12][on], under mu_)
    std::atomic<bool> cam_kick_browser_{false};   // send [0x21] from rtcPoll (lock order: rtc before mu_)
    bool camToBrowser(uint8_t pin, uint16_t fid, const uint8_t* jpeg, size_t len);
    uint16_t cam_fid_ = 0;
    std::mutex nonce_mu_;                // video is sent from the caller's thread
    uint64_t nextNonces(size_t n);       // n consecutive nonces, returns the first
    // Video send buffers, reused for every frame (zero allocations per frame).
    std::mutex cam_tx_mu_;
    void* cam_ctx_ = nullptr;            // EVP_CIPHER_CTX with the key set once
    std::vector<uint8_t> cam_pkts_;
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
    uint8_t kinds_[256] = {};            // declared type per pin (1 int, 2 float, 3 string, 4 bytes, 5 uint8, 6 double; 0 none)
    std::vector<uint16_t> last_links_;
    struct PinSum { uint32_t n = 0; float mn = 0, mx = 0, sum = 0, last = 0; };
    uint16_t relay_hz_ = RELAY_HZ_DEFAULT; // from the relay's peer list, under mu_ (0 = no limit)
    std::vector<sockaddr_in> stun_srv_;        // resolved STUN servers (under mu_)
    uint8_t stun_tid_[12] = {};
    std::vector<std::pair<uint32_t, uint16_t>> my_pub_;   // network order (under mu_)
    double last_stun_ = -1e9;
    std::atomic<bool> stun_busy_{false};
    bool relay_off_ = false;               // same list: no relay copy while all peers are direct
    std::map<uint8_t, std::string> relay_tx_;   // latest entry per pin for the relay, under mu_
    std::map<uint8_t, PinSum> sum_;      // 1-minute history summary ('M'), under mu_

    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> rx_direct_{0}, rx_relay_{0};
    std::atomic<double> last_rx_server_{0};
    std::vector<uint16_t> relay_ports_;   // see setRelayPorts
    size_t port_idx_ = 0;
    double port_since_ = 0;
    double last_reg_ = -10, last_links_sent_ = 0, last_pins_ = -100, last_summary_ = 0, last_relay_tx_ = 0;

    std::thread thread_;
    std::atomic<bool> stop_{false};
    // Short helper threads (allowed-pins HTTP, STUN lookups) use the members
    // above, so ~Client waits for them instead of letting them outlive it.
    void spawn(std::function<void()> fn);
    std::atomic<int> helpers_{0};
    std::atomic<bool> pins_busy_{false};
};

} // namespace relink
