/**
 * RelinkIoT Arduino Library v2.0.0
 * ESP32 / ESP8266 — UDP transport, current Relink server protocol
 *
 * Protocol (matches Relink server / Linux library):
 *   Device → Server  registration : '@' + token + '\n'
 *   Device → Server  VPin bulk    : 'B'[1][pin][dtype][val...][crc32-LE]
 *   Server → Device  SET_PIN relay: 'S'[pin][dtype][val...]
 *   Server → Device  heartbeat    : '@'[flipbit]'\n'
 *
 * Usage -- the token and encryption key from the device page:
 *   Relink.config("ssid", "password");
 *   Relink.begin(DEVICE_TOKEN, DEVICE_ENC_KEY, "server-ip");
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

// ── Version ───────────────────────────────────────────────────────────────────
#define RELINK_VERSION "2.1.0"

// ── Protocol data type tags ───────────────────────────────────────────────────
#define RELINK_TYPE_INT16  0
#define RELINK_TYPE_INT32  1
#define RELINK_TYPE_FLOAT  2
#define RELINK_TYPE_STRING 3

// ── Limits ────────────────────────────────────────────────────────────────────
#ifndef RELINK_API_PORT
#define RELINK_API_PORT 8000          // FastAPI port on the same host as the relay
#endif
#ifndef RELINK_WEBRTC_PORT
#define RELINK_WEBRTC_PORT 17453      // local UDP port for the P2P data channel
#endif
#ifndef RELINK_MESH_PEERS
#define RELINK_MESH_PEERS 4     // other programs on the same token reached directly
#endif
#ifndef RELINK_RX_BUF
#define RELINK_RX_BUF 1472            // one full Ethernet-MTU UDP payload
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

// ── VPin alias macros ─────────────────────────────────────────────────────────
#define V0   0
#define V1   1
#define V2   2
#define V3   3
#define V4   4
#define V5   5
#define V6   6
#define V7   7
#define V8   8
#define V9   9
#define V10  10
#define V11  11
#define V12  12
#define V13  13
#define V14  14
#define V15  15
#define V16  16
#define V17  17
#define V18  18
#define V19  19
#define V20  20
#define V21  21
#define V22  22
#define V23  23
#define V24  24
#define V25  25
#define V26  26
#define V27  27
#define V28  28
#define V29  29
#define V30  30
#define V31  31
#define V32  32
#define V33  33
#define V34  34
#define V35  35
#define V36  36
#define V37  37
#define V38  38
#define V39  39
#define V40  40
#define V41  41
#define V42  42
#define V43  43
#define V44  44
#define V45  45
#define V46  46
#define V47  47
#define V48  48
#define V49  49
#define V50  50
#define V51  51
#define V52  52
#define V53  53
#define V54  54
#define V55  55
#define V56  56
#define V57  57
#define V58  58
#define V59  59
#define V60  60
#define V61  61
#define V62  62
#define V63  63
#define V64  64
#define V65  65
#define V66  66
#define V67  67
#define V68  68
#define V69  69
#define V70  70
#define V71  71
#define V72  72
#define V73  73
#define V74  74
#define V75  75
#define V76  76
#define V77  77
#define V78  78
#define V79  79
#define V80  80
#define V81  81
#define V82  82
#define V83  83
#define V84  84
#define V85  85
#define V86  86
#define V87  87
#define V88  88
#define V89  89
#define V90  90
#define V91  91
#define V92  92
#define V93  93
#define V94  94
#define V95  95
#define V96  96
#define V97  97
#define V98  98
#define V99  99
#define V100 100
#define V101 101
#define V102 102
#define V103 103
#define V104 104
#define V105 105
#define V106 106
#define V107 107
#define V108 108
#define V109 109
#define V110 110
#define V111 111
#define V112 112
#define V113 113
#define V114 114
#define V115 115
#define V116 116
#define V117 117
#define V118 118
#define V119 119
#define V120 120
#define V121 121
#define V122 122
#define V123 123
#define V124 124
#define V125 125
#define V126 126
#define V127 127
#define V128 128
#define V129 129
#define V130 130
#define V131 131
#define V132 132
#define V133 133
#define V134 134
#define V135 135
#define V136 136
#define V137 137
#define V138 138
#define V139 139
#define V140 140
#define V141 141
#define V142 142
#define V143 143
#define V144 144
#define V145 145
#define V146 146
#define V147 147
#define V148 148
#define V149 149
#define V150 150
#define V151 151
#define V152 152
#define V153 153
#define V154 154
#define V155 155
#define V156 156
#define V157 157
#define V158 158
#define V159 159
#define V160 160
#define V161 161
#define V162 162
#define V163 163
#define V164 164
#define V165 165
#define V166 166
#define V167 167
#define V168 168
#define V169 169
#define V170 170
#define V171 171
#define V172 172
#define V173 173
#define V174 174
#define V175 175
#define V176 176
#define V177 177
#define V178 178
#define V179 179
#define V180 180
#define V181 181
#define V182 182
#define V183 183
#define V184 184
#define V185 185
#define V186 186
#define V187 187
#define V188 188
#define V189 189
#define V190 190
#define V191 191
#define V192 192
#define V193 193
#define V194 194
#define V195 195
#define V196 196
#define V197 197
#define V198 198
#define V199 199
#define V200 200
#define V201 201
#define V202 202
#define V203 203
#define V204 204
#define V205 205
#define V206 206
#define V207 207
#define V208 208
#define V209 209
#define V210 210
#define V211 211
#define V212 212
#define V213 213
#define V214 214
#define V215 215
#define V216 216
#define V217 217
#define V218 218
#define V219 219
#define V220 220
#define V221 221
#define V222 222
#define V223 223
#define V224 224
#define V225 225
#define V226 226
#define V227 227
#define V228 228
#define V229 229
#define V230 230
#define V231 231
#define V232 232
#define V233 233
#define V234 234
#define V235 235
#define V236 236
#define V237 237
#define V238 238
#define V239 239
#define V240 240
#define V241 241
#define V242 242
#define V243 243
#define V244 244
#define V245 245
#define V246 246
#define V247 247
#define V248 248
#define V249 249
#define V250 250
#define V251 251
#define V252 252
#define V253 253
#define V254 254
#define V255 255

// ── Callback types ────────────────────────────────────────────────────────────
typedef std::function<void()>                     RelinkCallback;
typedef std::function<void(int16_t)>              RelinkCallbackInt16;
typedef std::function<void(int32_t)>              RelinkCallbackInt32;
typedef std::function<void(float)>                RelinkCallbackFloat;
typedef std::function<void(const char*, uint8_t)> RelinkCallbackString;

// ── RelinkParam — unified value wrapper ──────────────────────────────────────
class RelinkParam {
public:
    RelinkParam(int16_t v)                : _t(0) { _u.i16 = v; }
    RelinkParam(int32_t v)                : _t(1) { _u.i32 = v; }
    RelinkParam(float v)                  : _t(2) { _u.f   = v; }
    RelinkParam(const char* s, uint8_t l) : _t(3), _s(s), _n(l) {}

    // Widgets like Control Slider always send FLOAT regardless of the pin's
    // configured data type, so a RELINK_WRITE(Vx) handler calling asInt()
    // must correctly convert a float-tagged param, not just reinterpret its
    // raw bits as an int32 (which is what happened before -- it only
    // "worked" at 0.0f because the all-zero bit pattern happens to also
    // read as integer 0).
    int16_t     asInt16()  const { return _t == 2 ? (int16_t)_u.f : _u.i16; }
    int32_t     asInt32()  const { return _t == 2 ? (int32_t)_u.f : _u.i32; }
    int         asInt()    const {
        if (_t == 2) return (int)_u.f;
        return (int)(_t == 0 ? _u.i16 : _u.i32);
    }
    float       asFloat()  const { return _t == 2 ? _u.f : (_t == 0 ? (float)_u.i16 : (float)_u.i32); }
    const char* c_str()    const { return _s ? _s : ""; }
    uint8_t     length()   const { return _n; }
    String      asString() const { return _s ? String(_s).substring(0, _n) : String(); }

private:
    uint8_t _t = 0;
    union { int16_t i16; int32_t i32; float f; } _u{};
    const char* _s = nullptr;
    uint8_t     _n = 0;
};

// ── VPin handler entry ────────────────────────────────────────────────────────
struct VPinHandler {
    uint8_t              type = 255;
    RelinkCallbackInt16  cbInt16;
    RelinkCallbackInt32  cbInt32;
    RelinkCallbackFloat  cbFloat;
    RelinkCallbackString cbString;
};

// ── RELINK_WRITE macro handler table ─────────────────────────────────────────
struct RelinkMacroHandler { void (*handler)(RelinkParam); uint8_t pin; };
extern RelinkMacroHandler relinkMacroHandlers[RELINK_MAX_MACRO_HANDLERS];
extern uint8_t            relinkMacroHandlerCount;
void relinkRegisterMacroHandler(uint8_t pin, void (*handler)(RelinkParam));

// ── Blynk-style macros ────────────────────────────────────────────────────────
// Use __attribute__((constructor)) — supported by GCC (avr-gcc, xtensa-gcc)
#define RELINK_WRITE(vPin) \
    void relinkHandler##vPin(RelinkParam param); \
    void __attribute__((constructor)) _relinkAutoReg##vPin() { \
        relinkRegisterMacroHandler(vPin, relinkHandler##vPin); \
    } \
    void relinkHandler##vPin(RelinkParam param)

#define RELINK_READ(vPin) RELINK_WRITE(vPin)

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

    // VPin read callbacks (dashboard → server → device)
    void onVirtualPin(uint8_t pin, RelinkCallbackInt16 cb);
    void onVirtualPin(uint8_t pin, RelinkCallbackInt32 cb);
    void onVirtualPin(uint8_t pin, RelinkCallbackFloat cb);
    void onVirtualPin(uint8_t pin, RelinkCallbackString cb);

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
    void _setKey(const char* keyHex);

    // Uplink batch: [count][pin][dtype][value]... (the 'B' / 'D' body)
    uint8_t _txBuf[RELINK_TX_BUF];
    size_t  _txLen = 0;
    bool    _txNeedsRelay = false;   // false: the batch is only for direct mesh peers (gateway)
    void _queueVPin(uint8_t pin, uint8_t type, const void* val, uint8_t sz);

    // OTA
    const char* _otaApiBase = nullptr;
    bool        _otaEnabled = false;

    // State
    bool _registered = false;
    bool _debugEnabled = false;

    // Timing
    unsigned long _startTime    = 0;
    unsigned long _lastReg      = 0;
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
    };
    MeshPeer      _mesh[RELINK_MESH_PEERS];
    uint16_t      _instance = 0;        // nonce instance: top 16 bits of every nonce
    unsigned long _lastLinks = 0;
    uint8_t       _lastLinkMask = 0;
    void _meshTick();
    void _meshPeers(const uint8_t* buf, int n);
    void _meshPacket(const uint8_t* buf, int n, IPAddress ip, uint16_t port);
    void _meshLinks(bool force);
    bool _meshAllUp() const;
#endif
    uint16_t _udpPort = 0;

#ifdef RELINK_HAS_WEBRTC
    rtc_session_t _rtcSession{};
    int           _rtcSockFd = -1;
    bool          _rtcActive = false;
    unsigned long _rtcBeginTime = 0;
    RelinkCallback _webrtcOnOpenCb;
    std::function<void(const uint8_t*, size_t)> _webrtcOnMessageCb;
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
