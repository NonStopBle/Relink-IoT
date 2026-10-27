/**
 * RelinkIoT Arduino Library v2.0.0
 * ESP32 / ESP8266 — UDP transport, current Relink server protocol
 * ESP8266: builds, but not yet tested on a real board.
 *
 * Protocol (matches Relink server / Linux library):
 *   Device → Server  registration : '@' + token + '\n'
 *   Device → Server  VPin bulk    : 'B'[1][pin][dtype][val...][crc32-LE]
 *   Server → Device  SET_PIN relay: 'S'[pin][dtype][val...]
 *   Server → Device  heartbeat    : '@'[flipbit]'\n'
 *
 * Usage -- the token and encryption key from the device page:
 *   Relink.config("ssid", "password");
 *   Relink.begin(DEVICE_TOKEN, DEVICE_ENC_KEY, "relink-iot.com");  // hostname or IP
 *
 * Everything else is automatic: the device id (CRC32 of the token), the
 * API base for OTA / P2P signaling (http://server-ip:RELINK_API_PORT) and
 * P2P itself (port RELINK_WEBRTC_PORT, random ICE password).
 *
 * A blank key ("" or nullptr) means no encryption. The server refuses
 * plaintext from a device that has a key, so that only connects after the
 * owner allows plaintext for it (POST /api/device/{id}/security).
 *
 *   void loop() {
 *     Relink.run();
 *     Relink.virtualWrite(V0, analogRead(A0));
 *   }
 *
 *   RELINK_WRITE(V1) {
 *     // called when server relays a value to V1
 *     int v = param.asInt();
 *   }
 */

#pragma once

#include <Arduino.h>
#include <functional>

// Included early (before the platform block below) so RELINK_HAS_WEBRTC is
// known in time to decide whether HTTPClient.h is needed even when OTA
// itself is disabled -- WebRTC's signaling poll (RelinkIoT.cpp,
// _webrtcCheckSignaling()) uses HTTPClient independently of OTA.
#ifdef ESP32
  extern "C" {
  #include "webrtc_lite_config.h"
  }
#endif

#ifdef ESP32
  #include <WiFi.h>
  #include <WiFiUdp.h>
  #if !defined(RELINK_NO_OTA) || defined(RELINK_HAS_WEBRTC)
    #include <HTTPClient.h>
  #endif
  #ifndef RELINK_NO_OTA
    #include <Update.h>
  #endif
  #ifndef RELINK_NO_ENCRYPTION
    // ChaCha20-Poly1305 (RFC 7539/8439, 12-byte nonce -- the same construction
    // as the server's OpenSSL EVP_chacha20_poly1305) via rweather/Crypto's
    // ChaChaPoly class. NOT via ESP-IDF's bundled mbedtls: the precompiled
    // Arduino-ESP32 core ships mbedtls with MBEDTLS_CHACHA20_C/POLY1305_C/
    // CHACHAPOLY_C all compiled out (empty .o's in libmbedcrypto.a, verified
    // by nm turning up zero symbols) -- there is no way to enable them
    // without rebuilding the SDK from source. rweather/Crypto needs adding
    // as a lib_deps entry: rweather/Crypto@^0.4.0
    //
    // See the "Encrypted transport" section below: this is what stops the
    // device's permanent token from ever appearing in a plaintext packet, so
    // sniffing the wire no longer hands over permanent impersonation
    // capability.
    #include <ChaChaPoly.h>
    #include <Preferences.h>
    #define RELINK_HAS_ENCRYPTION 1
  #endif
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <WiFiUdp.h>
  #ifndef RELINK_NO_OTA
    #include <ESP8266HTTPClient.h>
    #include <Updater.h>
  #endif
  #ifndef RELINK_NO_ENCRYPTION
    // Same rweather/Crypto ChaChaPoly as the ESP32 path -- it's a portable
    // Arduino library, not ESP32-specific, so it builds fine here too.
    // ESP8266's Arduino core has no Preferences/NVS equivalent, so the
    // nonce high-watermark is persisted via the emulated-flash EEPROM
    // library instead (already linked in for firmware-version storage).
    #include <ChaChaPoly.h>
    #define RELINK_HAS_ENCRYPTION 1
  #endif
#else
  #error "RelinkIoT supports ESP32 and ESP8266 only"
#endif

// WebRTC-lite data channel (direct browser<->device, bypassing the server
// relay once connected): ESP32 only. It needs mbedTLS's DTLS server path
// (cert generation, cookie context) and raw lwIP sockets for the STUN/DTLS
// demux, neither of which ESP8266's Arduino core provides the equivalent
// of. See webrtc_lite_config.h for the RELINK_NO_WEBRTC toggle -- same
// Arduino-IDE-compatible pattern as RELINK_NO_OTA/RELINK_NO_ENCRYPTION
// above (edit that header directly if you're not on PlatformIO).
#ifdef ESP32
  #ifdef RELINK_HAS_WEBRTC
    extern "C" {
    #include "rtc_session.h"
    }
  #endif
#endif

// Images / camera (virtualImageWrite, cameraApply): off unless asked for, so
// sketches without a camera carry none of it. ESP32 only; needs encryption
// (mesh peers) and/or WebRTC (dashboard). PlatformIO: build_flags =
// -DRELINK_CAMERA_STREAM. Arduino IDE: uncomment the line below (a #define
// in the sketch does not reach the library, see webrtc_lite_config.h).
// #define RELINK_CAMERA_STREAM
#if defined(RELINK_CAMERA_STREAM) && !defined(ESP32)
  #undef RELINK_CAMERA_STREAM
#endif
#ifdef RELINK_CAMERA_STREAM
  #include <esp_camera.h>
#endif

#include "RelinkTypes.h"

// ── Limits ────────────────────────────────────────────────────────────────────
#ifndef RELINK_API_PORT
#define RELINK_API_PORT 8000          // FastAPI port on the same host as the relay
#endif
#ifndef RELINK_WEBRTC_PORT
#define RELINK_WEBRTC_PORT 17453      // local UDP port for the P2P data channel
#endif
#ifdef RELINK_CAMERA_STREAM
#ifndef RELINK_CAM_CHUNK
#define RELINK_CAM_CHUNK 1024   // JPEG bytes per camera packet (DTLS record stays < RELINK_P2P_MAX_PACKET)
#endif
#ifndef RELINK_CAM_SUB_MS
#define RELINK_CAM_SUB_MS 3000  // a camera subscription lapses this long after its last refresh
#endif
#endif
#ifndef RELINK_MESH_PEERS
#define RELINK_MESH_PEERS 4     // other programs on the same token reached directly
#endif
#ifndef RELINK_RX_BUF
#define RELINK_RX_BUF 1472            // one full Ethernet-MTU UDP payload
#endif
// Live data over the relay (no P2P path): per pin, latest value wins, at the
// rate the relay announces in its peer list (admin setting on the server).
// This is only the value used until the relay has answered; 0 = no limit.
#ifndef RELINK_RELAY_HZ
#define RELINK_RELAY_HZ 100
#endif
// While P2P carries the dashboard's data the relay copy only feeds storage and
// other viewers: cap it so the radio isn't sending every sample twice.
#ifndef RELINK_RELAY_HZ_P2P
#define RELINK_RELAY_HZ_P2P 20
#endif
#ifndef RELINK_TX_BUF
#define RELINK_TX_BUF 1100            // batched VPin uplink payload (fits one MTU encrypted)
#endif
// Uplink pacing: run() sends a batch at most every _txInterval ms. It starts
// at MIN, doubles (up to MAX) each time the WiFi stack is full, and creeps
// back down while sends succeed -- as fast as the link takes, no faster.
// MAX + RELINK_TX_BACKOFF_MS bounds how long a value waits on the device
// (30 + 10 = 40 ms), leaving ~10 ms of a 50 ms budget for the network; under
// load the waiting value is replaced by newer ones instead of queuing.
#ifndef RELINK_TX_MIN_MS
#define RELINK_TX_MIN_MS 10
#endif
#ifndef RELINK_TX_MAX_MS
#define RELINK_TX_MAX_MS 30
#endif
#ifndef RELINK_TX_BACKOFF_MS
#define RELINK_TX_BACKOFF_MS 10
#endif

#ifndef RELINK_P2P_IDLE_MS
// Keepalive registration to the relay. NAT mappings last >= 30 s; the relay
// forgets an endpoint after 60 s and answers 'G' if it already has.
#ifndef RELINK_REGISTER_MS
#define RELINK_REGISTER_MS 25000UL
#endif
// After a relay restart ('G'), re-register at a random moment within this.
#ifndef RELINK_REJOIN_SPREAD_MS
#define RELINK_REJOIN_SPREAD_MS 5000UL
#endif
// History: min/max/mean/last of every numeric VPin, sent to the relay this often.
#ifndef RELINK_SUMMARY_MS
#define RELINK_SUMMARY_MS 60000UL
#endif
// Summaries not yet acked by the relay are kept here (oldest dropped when full)
// and sent once it answers again. ~100 B per summary for 5 pins = ~80 min.
#ifndef RELINK_SUMMARY_BUFFER
#ifdef ESP8266
#define RELINK_SUMMARY_BUFFER 2048    // ~20 min for 5 pins: ESP8266 has ~80 KB of RAM in all
#else
#define RELINK_SUMMARY_BUFFER 8192
#endif
#endif
#ifndef RELINK_SUMMARY_RETRY_MS
#define RELINK_SUMMARY_RETRY_MS 5000UL  // no ack this long = relay away, try again
#endif
#define RELINK_P2P_IDLE_MS 15000UL     // no packet from the browser this long = tab closed
#endif
#ifndef RELINK_P2P_FALLBACK_POLL_MS
#define RELINK_P2P_FALLBACK_POLL_MS 30000UL  // HTTP offer check when no 'W' nudge arrives (old servers)
#endif

#ifndef RELINK_P2P_PINS_REFRESH_MS
#define RELINK_P2P_PINS_REFRESH_MS 30000UL  // re-check datastream pins during a P2P session
#endif

#ifndef RELINK_MAX_VPINS
#define RELINK_MAX_VPINS 64          // raise if you need V0–V255 (costs RAM)
#endif
#ifndef RELINK_MAX_MACRO_HANDLERS
#define RELINK_MAX_MACRO_HANDLERS 32
#endif
// Buffer sizes for claim() output -- link_token is 32 chars + nul,
// enc_key_hex is 64 hex chars + nul.
#ifndef RELINK_CLAIM_TOKEN_LEN
#define RELINK_CLAIM_TOKEN_LEN 33
#endif
#ifndef RELINK_CLAIM_KEY_LEN
#define RELINK_CLAIM_KEY_LEN 65
#endif
// How long webrtcBegin() is allowed to keep trying ICE/DTLS/SCTP before
// giving up on P2P for this session. There's no TURN fallback in
// rtc_session (see rtc_session.h) -- a NAT that needs relay to traverse
// (symmetric NAT on either side, typically) will never complete the ICE
// handshake no matter how long we wait, so this bounds how long the
// device spends on an attempt that was never going to succeed. The
// server-relayed path (run()'s normal registration/heartbeat/VPin
// traffic) is never gated on this and keeps working the entire time,
// timeout or not -- "falling back to relay" needs no separate action,
// it's what's already happening whenever webrtcConnected() is false.
#ifndef RELINK_P2P_HEALTHY_MS
#define RELINK_P2P_HEALTHY_MS 2500UL   // browser silent this long = use the relay (session stays open)
#endif
#ifndef RELINK_WEBRTC_TIMEOUT_MS
#define RELINK_WEBRTC_TIMEOUT_MS 15000UL
#endif

// ── VPin handler entry ────────────────────────────────────────────────────────
struct VPinHandler {
    uint8_t              type = 255;
    RelinkCallbackInt16  cbInt16;
    RelinkCallbackInt32  cbInt32;
    RelinkCallbackFloat  cbFloat;
    RelinkCallbackString cbString;
    RelinkCallbackBytes  cbBytes;
};

// ── RELINK_WRITE macro handler table ─────────────────────────────────────────
struct RelinkMacroHandler { void (*handler)(RelinkParam); uint8_t pin; };
extern RelinkMacroHandler relinkMacroHandlers[RELINK_MAX_MACRO_HANDLERS];
extern uint8_t            relinkMacroHandlerCount;

// ── Main class ────────────────────────────────────────────────────────────────
class RelinkIoT {
public:
    RelinkIoT();

    // WiFi + server config — call before begin()
    void config(const char* ssid, const char* password);

    // Connect and start — call in setup(). encKeyHex is the 64-hex-char key
    // from the device page; blank ("" / nullptr) = no encryption.
    void begin(const char* token, const char* encKeyHex, const char* server, uint16_t port = 7445);
    // Legacy form: plaintext unless enableEncryption() was called first.
    void begin(const char* token, const char* server, uint16_t port = 7445);

    // Write several pins in one call -- they already share one packet:
    // every virtualWrite() between two run() calls is batched automatically.
    // flush() sends the batch right away instead of at the next run().
    void flush();

    // OTA config (optional, must call before begin())
    void configOTA(const char* apiBase = nullptr);
    void enableOTA(bool enable = true);

#ifndef RELINK_NO_OTA
    // Secure bootstrap: call once, at first boot, with the short-lived
    // single-use claim code shown in the dashboard when the device was
    // created (instead of hand-copying the permanent link_token/enc_key_hex
    // into firmware source, where a leaked repo or binary would hand over a
    // permanent credential). WiFi must already be connected (call after
    // config()+a manual WiFi.begin()/wait, or after Relink's own WiFi
    // connect if you call this from an onConnect() callback).
    //
    // On success, fills tokenOut/keyHexOut (caller-provided buffers, sized
    // per RELINK_CLAIM_TOKEN_LEN/RELINK_CLAIM_KEY_LEN below) and checksumOut,
    // and returns true -- caller still calls begin()/enableEncryption()
    // themselves with the results, exactly as if they'd been pasted in by
    // hand. Persist them yourself (Preferences on ESP32, EEPROM on ESP8266)
    // if you don't want to re-claim on every reboot -- the code is
    // single-use, so a second claim() call after the first reboot will fail.
    bool claim(const char* claimCode, const char* apiBase,
               char* tokenOut, size_t tokenOutLen,
               char* keyHexOut, size_t keyHexOutLen,
               uint32_t* checksumOut);
#endif

#ifdef RELINK_HAS_ENCRYPTION
    // Switches registration + VPin uploads from plaintext '@'/'B' frames to
    // the AEAD-encrypted 'E' envelope (ChaCha20-Poly1305) the server already
    // supports. Call before begin(). deviceId is any stable, non-secret u32
    // identifier your server-side provisioning assigned this device (the
    // Relink API returns one as "checksum" + "enc_key_hex" when a device is
    // created) -- NOT derived from the token itself. keyHex is the 64-hex-
    // char (32-byte) key as provisioned. Once enabled, the token never goes
    // on the wire in cleartext again, closing the "sniff one keepalive,
    // impersonate the device forever" hole plaintext mode has.
    // Older way to pass the key (call before the 3-argument begin()). The
    // device id is the checksum, which begin() now computes from the token.
    void enableEncryption(uint32_t deviceId, const char* keyHex);
#endif
    bool secure() const { return _secure; }

#ifdef RELINK_HAS_WEBRTC
    // Opt-in direct browser<->device data channel: once connected, data
    // flows peer-to-peer (through NAT via ICE, or not at all if that
    // fails -- there's no TURN fallback, see rtc_session.h) instead of
    // being relayed through the server.
    //
    // apiBase: the Relink API server (same one you'd pass to configOTA()),
    // used to poll /api/device/webrtc-offer/{token} and post
    // /api/device/webrtc-answer/{token} -- the one-shot signaling exchange
    // (see api/app.py) that hands you the browser's offer and carries your
    // answer back. Pass nullptr to skip signaling entirely and drive
    // webrtcAnswer()/the rest yourself against your own signaling channel.
    // udpPort: local port to listen on; becomes the a=candidate port in
    // the SDP answer this builds for you.
    // local_ice_pwd: becomes the SDP answer's a=ice-pwd.
    // Returns false if the socket couldn't be opened/bound.
    bool webrtcBegin(uint16_t udpPort, const char* local_ice_pwd, const char* apiBase = nullptr);

    // Drives ICE/DTLS/SCTP and delivers any received data channel messages
    // to the onmessage callback (set via webrtcOnMessage()). You don't
    // need to call this yourself -- run() calls it automatically once
    // webrtcBegin() has been called, and stops once the attempt either
    // connects or times out (RELINK_WEBRTC_TIMEOUT_MS above). Exposed
    // publicly only in case you're not calling run() in your loop().
    void webrtcPoll();

    bool webrtcSend(const uint8_t* data, size_t len);
    bool webrtcConnected();

    // True while still attempting the P2P handshake. Once it either
    // succeeds (webrtcConnected() becomes true) or gives up after timing
    // out, this returns false -- at that point the device is relying
    // entirely on the normal server-relayed path, exactly as if
    // webrtcBegin() had never been called.
    bool webrtcConnecting();

    // The DTLS certificate fingerprint for this boot, formatted exactly as
    // an SDP a=fingerprint:sha-256 line needs it ("AB:CD:...:EF") -- hand
    // this to whatever builds your SDP answer. Valid only after
    // webrtcBegin() succeeds; regenerated (and the old one invalidated)
    // every boot, since the cert is never persisted.
    const char* webrtcFingerprint();

    void webrtcOnOpen(RelinkCallback cb);
    void webrtcOnMessage(std::function<void(const uint8_t*, size_t)> cb);
#endif

#ifdef RELINK_CAMERA_STREAM
    // ── Images / camera (P2P only, never relayed) ───────────────────────
    // Sends one image (a JPEG, e.g. a camera frame) on V<pin> -- a Camera
    // datastream -- to whoever is watching right now: the dashboard's camera
    // widget over the direct WebRTC channel, and C++/Python programs on this
    // token that reach the device directly and asked for images. Nothing goes
    // through the server, and only one viewer gets it: whoever started
    // watching last (the previous one is told it was replaced). The image is
    // sliced straight out of your buffer into each packet -- no copy.
    //
    // Non-blocking: the image is handed to a sender task on the other core
    // and the call returns at once, so this loop can capture the next frame
    // while the previous one goes out (use fb_count >= 2). The camera frame
    // goes back to the driver when it's sent:
    //
    //   if (Relink.virtualImageWanted()) {
    //       camera_fb_t* fb = esp_camera_fb_get();
    //       if (fb && !Relink.virtualImageWrite(V10, fb)) esp_camera_fb_return(fb);
    //   }
    //
    // Returns false without sending (and without taking the frame) when
    // nobody watches, the previous image is still going out, or the max
    // frame rate (cameraApply) says wait.
    bool virtualImageWrite(uint8_t pin, camera_fb_t* fb);
    // Any other image buffer, non-blocking: release(ctx) runs on the sender
    // core once `data` is no longer read.
    bool virtualImageWrite(uint8_t pin, const uint8_t* data, size_t len, void (*release)(void*), void* ctx);
    // Blocking: returns once the image is sent; `data` is free again after.
    bool virtualImageWrite(uint8_t pin, const uint8_t* data, size_t len);
    // Someone is watching and a new image may go now (frame-rate limit
    // included) -- check it before capturing to skip unneeded captures.
    bool virtualImageWanted();
    bool virtualImageBusy() const { return _camBusy; }

    // Applies camera widget settings to the ESP32 camera sensor -- call it
    // from the settings pin's handler (a ByteArray datastream):
    //   RELINK_WRITE(V11) { Relink.cameraApply(param); }
    // Takes the widget's 10-byte record, or text such as "fs=QVGA;q=20;fps=15"
    // written from C++/Python. Sets resolution, JPEG quality, brightness,
    // contrast, saturation, mirror/flip, auto white balance/exposure,
    // exposure compensation, effect, and the max frame rate used by
    // virtualImageWanted(). Init the camera at the largest resolution you'll
    // pick (frame buffers are sized from it).
    void cameraApply(const RelinkParam& param);
#endif

    // Main loop — call every iteration of loop()
    void run();

    // Connection state
    bool connected();
    bool registered();

    // VPin write (device → server → dashboard)
    void virtualWrite(uint8_t pin, int16_t v);
    void virtualWrite(uint8_t pin, int32_t v);
    void virtualWrite(uint8_t pin, float v);
    void virtualWrite(uint8_t pin, const char* s);
    void virtualWrite(uint8_t pin, const String& s);
    void virtualWrite(uint8_t pin, const uint8_t* data, uint8_t len);   // ByteArray datastream

    // VPin read callbacks (dashboard → server → device)
    void onVirtualPin(uint8_t pin, RelinkCallbackInt16 cb);
    void onVirtualPin(uint8_t pin, RelinkCallbackInt32 cb);
    void onVirtualPin(uint8_t pin, RelinkCallbackFloat cb);
    void onVirtualPin(uint8_t pin, RelinkCallbackString cb);
    void onVirtualPin(uint8_t pin, RelinkCallbackBytes cb);

    // Lifecycle events
    void onConnect(RelinkCallback cb);
    void onDisconnect(RelinkCallback cb);
    void onRegistered(RelinkCallback cb);

    // Diagnostics
    void     setDebug(bool enable = true);
    uint32_t getUptime();
    float    getLatency() { return 0.0f; }
    int32_t  getFirmwareVersion();

private:
    // WiFi + server
    const char* _ssid     = nullptr;
    const char* _password = nullptr;
    const char* _token    = nullptr;
    const char* _server   = nullptr;
    uint16_t    _port     = 7445;
    bool        _secure   = false;
    bool        _netUp    = false;   // UDP socket set up for the current WiFi connection
    IPAddress   _netIp;
    unsigned long _lastWifiRetry = 0;
    void _onNetUp();
    void _onNetDown();
    unsigned long _txBackoffUntil = 0, _txDrops = 0, _lastDropLog = 0, _lastFlush = 0;
    uint16_t    _txInterval = RELINK_TX_MIN_MS;
    uint8_t     _txOk = 0;
    bool        _txFailedNow = false;
    void        _txFailed(IPAddress ip, uint16_t port);
    bool        _txBackoff();
    bool        _secureBroken = false;   // key given but unusable -- refuse to fall back to plaintext
    String      _autoApiBase;            // http://server:RELINK_API_PORT
    void _begin(const char* token, const char* server, uint16_t port);
    bool _resolveServer();
    void _setKey(const char* keyHex);

    // Uplink batch: [count][pin][dtype][value]... (the 'B' / 'D' body)
    uint8_t _txBuf[RELINK_TX_BUF];
    size_t  _txLen = 0;
    bool    _txNeedsRelay = false;   // false: the batch is only for direct mesh peers (gateway)
    void _queueVPin(uint8_t pin, uint8_t type, const void* val, uint8_t sz);
    uint8_t _relayBuf[RELINK_TX_BUF];    // latest value per pin for the relay, sent at RELINK_RELAY_HZ
    size_t  _relayLen = 0;
    unsigned long _lastRelayTx = 0;
    uint16_t _relayHz = RELINK_RELAY_HZ;
    uint16_t _relayRate();
    bool _relayDue() { uint16_t hz = _relayRate(); return !hz || millis() - _lastRelayTx >= 1000UL / hz; }
    void _relayMerge();
    void _sendRelay();

    // OTA
    const char* _otaApiBase = nullptr;
    bool        _otaEnabled = false;

    // State
    bool _registered = false;
    bool _debugEnabled = false;

    // Timing
    unsigned long _startTime    = 0;
    unsigned long _lastReg      = 0;
    unsigned long _regItv       = 500;     // retry wait until registered
    bool          _meshLinksDue = false;   // send links with the next registration
    unsigned long _lastHeartbeat= 0;
    unsigned long _lastOtaCheck = 0;
    bool          _otaReported  = false;
    int32_t       _otaFailedVer = 0;
    volatile bool _otaWake      = false;
    volatile int32_t _otaPendingVer = 0;   // from a 'V' reply, applied in run()
    long          _otaPendingFs  = 0;
    String        _otaError;

    // UDP
    WiFiUDP _udp;
    IPAddress _serverIp;

    // Handlers
    VPinHandler    _vpinHandlers[RELINK_MAX_VPINS];
    RelinkCallback _onConnectCb;
    RelinkCallback _onDisconnectCb;
    RelinkCallback _onRegisteredCb;

    // Recv buffer
    uint8_t _rxBuf[RELINK_RX_BUF];

#ifdef RELINK_HAS_ENCRYPTION
    // Encrypted-transport state
    bool     _encEnabled = false;
    uint32_t _deviceId   = 0;
    uint8_t  _encKey[32] = {};
    // Nonce counter must never repeat for (key, device_id). Persisted to NVS
    // in RELINK_NONCE_PERSIST_EVERY-sized blocks so a reboot jumps straight
    // past the highest value this device could plausibly have sent before
    // losing power, instead of restarting at 0 and getting every packet
    // rejected as a replay by a server that still remembers the old high
    // watermark (last_nonce is in-memory on the server, so this only
    // matters within a single relink_udp uptime, but costs nothing to get
    // right unconditionally).
    uint64_t _nonceCounter = 0;
#ifdef ESP32
    Preferences _encPrefs;
#endif
    void _loadPersistedNonce();
    void _persistNonce();
    void _sendEncrypted(uint8_t subtype, const uint8_t* inner, size_t innerLen);
    void _sendEncryptedTo(uint8_t subtype, const uint8_t* inner, size_t innerLen, IPAddress ip, uint16_t port);

    // Mesh: other programs on this device token (Python, C++, another
    // board). The relay tells us where they are ('N'); we say hello directly
    // and, once one answers, send it pin writes peer-to-peer (encrypted with
    // the same key) and tell the relay to stop forwarding to it ('L').
    struct MeshPeer {
        bool used = false;
        uint16_t inst = 0;
        IPAddress pub, lan, addr;
        uint16_t pubPort = 0, lanPort = 0, addrPort = 0;
        unsigned long lastRx = 0, lastHello = 0;
        uint64_t lastNonce = 0;
        bool up() const { return addrPort && millis() - lastRx < 5000UL; }
#ifdef RELINK_CAMERA_STREAM
        unsigned long camRx = 0;   // last 'C' (camera subscribe) from this peer
        bool cam() const { return up() && camRx && millis() - camRx < RELINK_CAM_SUB_MS; }
#endif
    };
    MeshPeer      _mesh[RELINK_MESH_PEERS];
#ifdef RELINK_CAMERA_STREAM
    uint16_t      _camFrame = 0;
#endif
    uint16_t      _instance = 0;        // nonce instance: top 16 bits of every nonce
    unsigned long _lastLinks = 0;
    uint8_t       _lastLinkMask = 0;
    uint8_t       _lastMeshFlags = 0;
    void _meshTick();
    void _meshPeers(const uint8_t* buf, int n);
    void _meshPacket(const uint8_t* buf, int n, IPAddress ip, uint16_t port);
    void _meshLinks(bool force);
    bool _meshAllUp() const;
    uint8_t _meshFlags();

    // 1-minute summary per VPin ('M' to the relay), so history survives
    // while live data skips the relay.
    struct PinSum { uint16_t n = 0; float mn, mx, sum, last; };
    PinSum        _sum[RELINK_MAX_VPINS];
    unsigned long _lastSummary = 0;
    void _sumAdd(uint8_t pin, uint8_t type, const void* val);
    void _sendSummary();
    // Unacked summaries, oldest first: records [len:u16][seq:u16][t_ms:u32][body].
    uint8_t       _sumBuf[RELINK_SUMMARY_BUFFER];
    size_t        _sumBufLen = 0;
    uint16_t      _sumSeq = 0;
    unsigned long _sumSentAt = 0;
    bool          _sumInFlight = false;
    void _sumFlush();
    void _sumAcked(uint16_t seq);
#endif
    uint16_t _udpPort = 0;

#ifdef RELINK_CAMERA_STREAM
    // Image sender (see virtualImageWrite). A job is filled in by the loop
    // task, then each core sends its half of the destinations.
    // One viewer at a time: whoever starts watching last owns the stream
    // (the browser, or one mesh peer) and the previous owner is told it was
    // replaced -- all of the link goes to one receiver.
    static constexpr int8_t CAM_NONE = -1, CAM_BROWSER = 100;   // else: mesh slot
    int8_t        _camOwner = CAM_NONE;
    uint16_t      _camOwnerInst = 0;      // mesh owner's instance (slots get reused)
    void _camTake(int8_t owner);
    struct CamJob {
        const uint8_t* jpeg = nullptr;
        size_t len = 0;
        uint16_t fid = 0, cnt = 0;
        uint8_t pin = 0;
        int fd = -1;
        bool mesh = false;                    // owner is a mesh peer: both cores send to it
        uint32_t ip = 0; uint16_t port = 0;   // its address, network order
        uint64_t nonce = 0;                   // first of cnt nonces reserved for this frame
        void (*release)(void*) = nullptr;
        void* ctx = nullptr;
    };
    CamJob            _camJob;
    volatile bool     _camBusy = false;
    void*             _camTask = nullptr;     // TaskHandle_t
    int  _camUdpFd();
    void _camSendMesh(uint8_t* pkt);
    void _camSendBrowser();
    void* _rtcMu = nullptr;               // SemaphoreHandle_t: the sender task shares the WebRTC session
    static void _camTaskFn(void* self);
    uint16_t      _camMaxFps = 0;         // from cameraApply; 0 = no limit
    unsigned long _camLastSend = 0;
    // Per-stage timing, printed every 2 s with RELINK_DEBUG (us totals).
    struct CamStats { uint32_t frames, bytes, sendUs, lockUs, gapUs, retries; int64_t idleAt, printAt; };
    CamStats _camStats{};
    void _camStatsPrint();
    bool _imageWatched();
    void _cameraApplyRecord(const uint8_t* r, size_t len);
    void _cameraApplyText(const char* str, size_t len);
#endif

#ifdef RELINK_HAS_WEBRTC
    rtc_session_t _rtcSession{};
    int           _rtcSockFd = -1;
    bool          _rtcActive = false;
    unsigned long _rtcBeginTime = 0;
    RelinkCallback _webrtcOnOpenCb;
    std::function<void(const uint8_t*, size_t)> _webrtcOnMessageCb;
#ifdef RELINK_CAMERA_STREAM
    bool          _camBrowser = false;   // dashboard has a camera widget open (0x12)
#endif
    static void _webrtcOnOpenTrampoline(void* user);
    static void _webrtcOnMessageTrampoline(void* user, const uint8_t* data, size_t len);

    // Signaling (one-shot offer/answer against api/app.py's
    // /api/device/webrtc-{offer,answer}/{token} endpoints)
    const char*   _webrtcApiBase = nullptr;
    const char*   _rtcIcePwd     = nullptr;
    uint16_t      _rtcUdpPort    = 0;
    bool          _rtcAnswered   = false;
    bool          _rtcAutoTried  = false;
    bool          _rtcEnabled    = false;   // webrtcBegin() ran: keep answering new dashboards
    bool          _rtcWake       = false;   // relay sent 'W': an offer is waiting
    bool          _rtcReadOnly   = false;   // offer came from a view-only share link: no pin writes
    unsigned long _rtcAnswerTime = 0;
    char          _rtcAutoPwd[33] = {0};
    bool _rtcOpenSession();
    void _rtcCloseSession(const char* why);
    // P2P batch: [102][count] then [pin][dtype][len][value] (webapp protocol.ts PIN_BATCH)
    uint8_t _p2pBuf[1200];
    size_t  _p2pLen = 0;
    unsigned long _lastP2PSend = 0;
    void _queueP2P(uint8_t pin, uint8_t type, const void* val, uint8_t sz);
    void _flushP2P(bool force);
    bool _p2pHealthy();
    unsigned long _lastWebrtcSignalCheck = 0;
    void _webrtcCheckSignaling();

    // Datastream pins allowed on the direct P2P channel (the relay enforces
    // its own copy server-side, but P2P bypasses it). Delivered with the SDP
    // offer and refreshed every RELINK_P2P_PINS_REFRESH_MS while connected.
    // Unknown (old server) -> VPin traffic stays on the relay instead.
    uint8_t       _p2pPinMask[32] = {0};
    bool          _p2pPinsKnown   = false;
    unsigned long _p2pPinsFetched = 0;
    bool _p2pPinAllowed(uint8_t pin) const { return _p2pPinsKnown && (_p2pPinMask[pin >> 3] & (1u << (pin & 7))); }
    void _p2pParsePins(const char* csv);
    void _webrtcRefreshPins();
#endif

    // Internal
    void     _connectWiFi();
    void     _sendRegistration();
    void     _sendVPin(uint8_t pin, uint8_t type, const void* val, uint8_t sz);
    void     _pollRecv();
    void     _processFrame(const uint8_t* data, int len);
    void     _dispatchVPin(uint8_t pin, uint8_t type, const uint8_t* val, uint8_t vsz);
    void     _checkOTA();
    void     _applyOTA(int32_t serverVer, long fsSize);
    void     _otaSend(const String& msg);
    bool     _otaFlash(const char* part, int command);
    void     _otaReportProgress(const char* part, int written, int total);
    uint32_t _crc32(const uint8_t* d, size_t len);
    void     _debug(const char* fmt, ...);
};

extern RelinkIoT Relink;
