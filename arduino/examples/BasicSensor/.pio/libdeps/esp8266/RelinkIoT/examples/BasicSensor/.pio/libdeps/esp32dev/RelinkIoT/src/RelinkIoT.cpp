#ifdef ESP32
#include <esp_timer.h>
#endif
#include "RelinkIoT.h"
#include <EEPROM.h>
#ifdef RELINK_CAMERA_STREAM
#include <lwip/sockets.h>
#include <freertos/semphr.h>
#endif

// The image sender task (other core) shares the WebRTC session with run().
#if defined(RELINK_CAMERA_STREAM) && defined(RELINK_HAS_WEBRTC)
  #define RTC_LOCK()   do { if (_rtcMu) xSemaphoreTakeRecursive((SemaphoreHandle_t)_rtcMu, portMAX_DELAY); } while (0)
  #define RTC_UNLOCK() do { if (_rtcMu) xSemaphoreGiveRecursive((SemaphoreHandle_t)_rtcMu); } while (0)
#else
  #define RTC_LOCK()   do {} while (0)
  #define RTC_UNLOCK() do {} while (0)
#endif

// ── Global instance + macro table ────────────────────────────────────────────
RelinkIoT Relink;
RelinkMacroHandler relinkMacroHandlers[RELINK_MAX_MACRO_HANDLERS];
uint8_t            relinkMacroHandlerCount = 0;

void relinkRegisterMacroHandler(uint8_t pin, void (*handler)(RelinkParam)) {
    if (!handler || relinkMacroHandlerCount >= RELINK_MAX_MACRO_HANDLERS) return;
    relinkMacroHandlers[relinkMacroHandlerCount].pin     = pin;
    relinkMacroHandlers[relinkMacroHandlerCount].handler = handler;
    relinkMacroHandlerCount++;
}

// ── EEPROM layout ─────────────────────────────────────────────────────────────
// firmware version : bytes 0-3  (4 bytes)
// enc nonce (ESP8266 only; ESP32 uses Preferences/NVS instead) : bytes 4-11 (8 bytes)
#define EEPROM_SIZE        16
#define EEPROM_FW_ADDR     0
#define EEPROM_NONCE_ADDR  4

// ── Ctor ──────────────────────────────────────────────────────────────────────
RelinkIoT::RelinkIoT() {
    for (int i = 0; i < RELINK_MAX_VPINS; i++) _vpinHandlers[i].type = 255;
}

// ── CRC32 (poly 0xEDB88320) ───────────────────────────────────────────────────
uint32_t RelinkIoT::_crc32(const uint8_t* d, size_t len) {
    static uint32_t tbl[256] = {};
    static bool     init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tbl[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) c = tbl[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ── config ────────────────────────────────────────────────────────────────────
void RelinkIoT::config(const char* ssid, const char* password) {
    _ssid     = ssid;
    _password = password;
}

void RelinkIoT::configOTA(const char* apiBase) {
    _otaApiBase = apiBase;
}

void RelinkIoT::enableOTA(bool enable) {
#ifdef RELINK_NO_OTA
    if (enable) _debug("[OTA] enableOTA(true) ignored: built with RELINK_NO_OTA");
#endif
    if (enable != _otaEnabled) _otaReported = false;  // tell the server on the next run()
    _otaEnabled = enable;
}

// ── begin ─────────────────────────────────────────────────────────────────────
void RelinkIoT::begin(const char* token, const char* encKeyHex, const char* server, uint16_t port) {
    _token = token;
    _setKey(encKeyHex);
    _begin(token, server, port);
}

void RelinkIoT::begin(const char* token, const char* server, uint16_t port) {
#ifdef RELINK_HAS_ENCRYPTION
    _secure = _encEnabled;
#endif
    _begin(token, server, port);
}

void RelinkIoT::_begin(const char* token, const char* server, uint16_t port) {
    _token  = token;
    _server = server;
    _port   = port;
    _autoApiBase = String("http://") + server + ":" + RELINK_API_PORT;
    if (!_otaApiBase) _otaApiBase = _autoApiBase.c_str();

    EEPROM.begin(EEPROM_SIZE);
    _startTime = millis();

    if (_debugEnabled) {
        Serial.printf("\n=== RelinkIoT v%s ===\n", RELINK_VERSION);
        Serial.printf("[relink] server=%s:%u  security=%s\n", server, port,
                      _secureBroken ? "KEY UNUSABLE (not connecting)" : _secure ? "encrypted" : "plaintext (no key)");
    }

    _connectWiFi();   // only if config() gave credentials; never blocks
}

// Blank key -> plaintext. Otherwise the key must be usable: a typo'd key or
// a build without crypto never silently falls back to sending the token in
// the clear. Device id = CRC32(token) = the checksum the server keys it by.
void RelinkIoT::_setKey(const char* keyHex) {
    _secure = keyHex && *keyHex;
    _secureBroken = false;
    if (!_secure) return;
#ifdef RELINK_HAS_ENCRYPTION
    enableEncryption(_crc32((const uint8_t*)_token, strlen(_token)), keyHex);
    if (!_encEnabled) {
        _secureBroken = true;
        _debug("[relink] encryption key must be 64 hex chars -- not connecting");
    }
#else
    _secureBroken = true;
    _debug("[relink] an encryption key was given but encryption isn't compiled in "
           "(RELINK_NO_ENCRYPTION, or rweather/Crypto missing) -- not connecting");
#endif
}

// ── WiFi ──────────────────────────────────────────────────────────────────────
// The sketch owns WiFi: connect however you like (WiFi.begin + events,
// WiFiManager, static IP, ...) and Relink follows -- run() sets the UDP
// socket up the moment WiFi has an IP, re-registers after every reconnect
// or IP change, and pauses while it is down. config() is optional: with
// credentials, Relink calls WiFi.begin() for you (non-blocking) and retries.
void RelinkIoT::_connectWiFi() {
    if (!_ssid || !_password) return;
    _lastWifiRetry = millis();
    WiFi.mode(WIFI_STA);
    WiFi.begin(_ssid, _password);
    _debug("[relink] connecting WiFi '%s'...", _ssid);
}

// server may be an IP literal or a hostname (e.g. relink-iot.com). A failed
// DNS lookup keeps the last good address; registration retries while unset.
bool RelinkIoT::_resolveServer() {
    IPAddress ip;
    if (!ip.fromString(_server) && !WiFi.hostByName(_server, ip)) {
        _debug("[relink] DNS lookup failed for %s", _server);
        return (uint32_t)_serverIp != 0;
    }
    _serverIp = ip;
    return true;
}

void RelinkIoT::_onNetUp() {
    _netUp = true;
    _netIp = WiFi.localIP();
    _debug("[relink] network up, IP=%s", _netIp.toString().c_str());
    _resolveServer();
    _udp.stop();
    // Fresh port for this connection, known to us so peers can be told it.
    _udpPort = 20000 + (uint16_t)(random(0, 40000));
    _udp.begin(_udpPort);
#ifdef ESP32
    // Modem sleep holds outgoing frames until the next beacon; at 100 Hz the
    // stack runs out of buffers (endPacket errno 12) long before it drains.
    WiFi.setSleep(false);
#endif
    _registered = false;
    // First registration 0-2 s in: a power cut brings a whole site back at once.
    _regItv = 500UL;
    _lastReg = millis() - _regItv + (unsigned long)random(0, 2000);
    if (_onConnectCb) _onConnectCb();
#ifdef RELINK_HAS_WEBRTC
    _rtcWake = true;   // a dashboard may have been waiting while we were offline
#endif
}

void RelinkIoT::_onNetDown() {
    _netUp = false;
    _registered = false;
    _txLen = 0;
    _debug("[relink] network down");
#ifdef RELINK_HAS_WEBRTC
    if (_rtcActive) _rtcCloseSession("network down");
#endif
    if (_onDisconnectCb) _onDisconnectCb();
}

// ── run ───────────────────────────────────────────────────────────────────────
void RelinkIoT::run() {
    if (!_token) return;   // begin() not called yet
    if (WiFi.status() != WL_CONNECTED) {
        if (_netUp) _onNetDown();
        if (_ssid && millis() - _lastWifiRetry > 10000UL) _connectWiFi();
        return;
    }
    if (!_netUp || WiFi.localIP() != _netIp) _onNetUp();

    if (_secureBroken) return;

    // Re-register: until acked, retry from 0.5 s doubling to 8 s, each wait
    // randomised +-50% so a fleet doesn't retry in step; then every
    // RELINK_REGISTER_MS as keepalive.
    unsigned long itv = _registered ? RELINK_REGISTER_MS : _regItv;
    if (millis() - _lastReg >= itv) {
        _sendRegistration();
        _lastReg = millis();
#ifdef RELINK_HAS_ENCRYPTION
        if (_meshLinksDue) { _meshLinksDue = false; _meshLinks(true); }
#endif
        if (!_registered) {
            unsigned long base = _regItv < 4000UL ? _regItv * 2 : 8000UL;
            _regItv = base / 2 + (unsigned long)random(0, base);
        }
    }

    _pollRecv();

#ifndef RELINK_NO_OTA
    // OTA check = one small UDP report ('U') to the relay, which answers
    // with the server's firmware version ('V'). It also tells the dashboard
    // whether OTA is on, so it can refuse uploads a device won't apply.
    // An 'O' nudge from the relay checks at once; the timer is only a
    // fallback for a lost nudge. Disabled devices re-report every 10 min.
    unsigned long otaItv = !_otaReported ? 10000UL : 600000UL;
    if (_registered && (_otaWake || _lastOtaCheck == 0 || millis() - _lastOtaCheck > otaItv)) {
        _otaWake = false;
        _lastOtaCheck = millis();
        _checkOTA();
    }
    if (_otaPendingVer) {                 // set by a 'V' reply; flash outside the recv path
        int32_t v = _otaPendingVer;
        long fs = _otaPendingFs;
        _otaPendingVer = 0;
        _applyOTA(v, fs);
    }
#endif

#ifdef RELINK_HAS_ENCRYPTION
    _meshTick();
    if (millis() - _lastSummary >= RELINK_SUMMARY_MS) _sendSummary();
    _sumFlush();
#endif
    // Everything virtualWrite() queued since the last send, paced (see
    // RELINK_TX_MIN_MS). While waiting, rewrites of a pin replace its queued
    // value, so a slow link sends fewer, fresher batches instead of a backlog.
    if (millis() - _lastFlush >= _txInterval && !_txBackoff()) flush();
    if (_relayLen && _relayDue()) _sendRelay();

#ifdef RELINK_HAS_WEBRTC
    // P2P starts by itself once registered; a sketch that called
    // webrtcBegin() itself keeps its own settings.
    if (_registered && !_rtcAutoTried) {
        _rtcAutoTried = true;
        for (int i = 0; i < 32; i++) _rtcAutoPwd[i] = "abcdefghijklmnopqrstuvwxyz0123456789"[esp_random() % 36];
        webrtcBegin(RELINK_WEBRTC_PORT, _rtcAutoPwd, _autoApiBase.c_str());
    }
    // Opportunistic: tries P2P for up to RELINK_WEBRTC_TIMEOUT_MS after
    // webrtcBegin(), then stops on its own (see webrtcPoll()). The
    // registration/heartbeat/VPin relay traffic above runs unconditionally
    // either way, so this never blocks or delays the relay path.
    webrtcPoll();
#endif
}

// ── registration ──────────────────────────────────────────────────────────────
void RelinkIoT::_sendRegistration() {
    if (!_token || WiFi.status() != WL_CONNECTED) return;
    if ((uint32_t)_serverIp == 0 && !_resolveServer()) return;

#ifdef RELINK_HAS_ENCRYPTION
    if (_encEnabled) {
        // Inner subtype 'R' = ENC_INNER_REGISTER in protocol.hpp -- payload
        // is the raw token, same as the plaintext path, just never visible
        // on the wire. This is the fix for the sniff-the-keepalive attack:
        // with encryption on, the token literally never leaves the device
        // in cleartext again.
        // token '\0' caps -- 'M': this library parses batched downlink frames
        // ... '\0' local ip:port, so peers on the same LAN can reach us directly.
        uint8_t reg[128];
        size_t tl = strlen(_token);
        if (tl > 96) tl = 96;
        memcpy(reg, _token, tl); reg[tl] = 0; reg[tl + 1] = 'M'; reg[tl + 2] = 0;
        int ll = snprintf((char*)reg + tl + 3, sizeof(reg) - tl - 3, "%s:%u",
                          WiFi.localIP().toString().c_str(), _udpPort);
        _sendEncrypted('R', reg, tl + 3 + (ll > 0 ? ll : 0));
        _debug("[relink] encrypted registration sent");
        if (!_registered) {
            _registered = true;
            _debug("[relink] registered");
            if (_onRegisteredCb) _onRegisteredCb();
        }
        return;
    }
#endif

    // Protocol: '@' + token + '\n'
    if (_secure) return;   // has a key: the token never goes out in the clear
    _udp.beginPacket(_serverIp, _port);
    _udp.write('@');
    _udp.write((const uint8_t*)_token, strlen(_token));
    _udp.write((uint8_t)0); _udp.write('M');   // capabilities, see above
    _udp.write('\n');
    _udp.endPacket();

    _debug("[relink] registration sent");

    // Server sends no explicit ACK — mark registered after first send
    if (!_registered) {
        _registered = true;
        _debug("[relink] registered");
        if (_onRegisteredCb) _onRegisteredCb();
    }
}

// ── VPin write ────────────────────────────────────────────────────────────────
void RelinkIoT::virtualWrite(uint8_t pin, int16_t v)  { _sendVPin(pin, RELINK_TYPE_INT16,  &v, 2); }
void RelinkIoT::virtualWrite(uint8_t pin, int32_t v)  { _sendVPin(pin, RELINK_TYPE_INT32,  &v, 4); }
void RelinkIoT::virtualWrite(uint8_t pin, float v)    { _sendVPin(pin, RELINK_TYPE_FLOAT,  &v, 4); }
void RelinkIoT::virtualWrite(uint8_t pin, const char* s) {
    uint8_t n = (uint8_t)strnlen(s, 58);
    _sendVPin(pin, RELINK_TYPE_STRING, s, n);
}
void RelinkIoT::virtualWrite(uint8_t pin, const String& s) {
    virtualWrite(pin, s.c_str());
}
void RelinkIoT::virtualWrite(uint8_t pin, const uint8_t* data, uint8_t len) {
    _sendVPin(pin, RELINK_TYPE_BYTES, data, len > 58 ? 58 : len);   // same cap as strings
}

// ── Internal: build and send bulk packet ──────────────────────────────────────
void RelinkIoT::_sendVPin(uint8_t pin, uint8_t type, const void* val, uint8_t sz) {
    if (!_registered || WiFi.status() != WL_CONNECTED) return;
#ifdef RELINK_HAS_ENCRYPTION
    _sumAdd(pin, type, val);
#endif

#ifdef RELINK_HAS_WEBRTC
    // Every VPin update goes over the direct device<->browser P2P data
    // channel INSTEAD OF the UDP relay whenever punching succeeded --
    // that's the whole point of the hot path (server never touches the
    // bytes). webrtcConnected() only ever becomes true if the real
    // STUN/DTLS/SCTP handshake completed, so this is a hard replacement,
    // not a mirror: the frame below is NOT also sent over UDP.
    //
    // Queued into one S2C.PIN_BATCH message per flush() (webapp
    // protocol.ts forEachPinFrame) -- the browser is the other end of this
    // data channel, not the server, so the frame is in its format.
    //
    // Deliberately sent PLAINTEXT at this layer, skipping the
    // RELINK_HAS_ENCRYPTION ('D'/ChaCha20-Poly1305) block below even when
    // enableEncryption() is on: the data channel itself is already
    // DTLS 1.2-secured end-to-end (see dtls_lite.c) before SCTP/DCEP ever
    // see these bytes, so a second application-layer AEAD would just be
    // redundant cost with no additional confidentiality/integrity.
    // Pins that aren't declared datastreams never go out over P2P. With the
    // list unknown (old server) the frame takes the relay below instead,
    // where relink_udp applies the same rule.
    if (_p2pHealthy() && _p2pPinsKnown && !_p2pPinAllowed(pin)) return;
    if (_p2pHealthy() && _p2pPinsKnown) {
        _queueP2P(pin, type, val, sz);
#ifdef RELINK_HAS_ENCRYPTION
        // Gateway: mesh peers (Python/C++) still get it directly; the relay
        // only if some peer isn't reachable directly (see flush()).
        if (_encEnabled) _queueVPin(pin, type, val, sz);
#endif
        return;
    }
#endif

    _txNeedsRelay = true;
    _queueVPin(pin, type, val, sz);
}

// ── Uplink batching ──────────────────────────────────────────────────────────
// Writes between two run() calls share one packet: 50 pins at 100 Hz is 100
// packets/s instead of 5000, which is what an ESP32's WiFi stack can keep up
// with (and the server's 'B'/'D' frames always carried a pin count).
// Adds [pin][type][(len)][value] to a [count][entries...] batch, overwriting
// the pin's queued value if it has the same type and size. false = full.
static bool batchPut(uint8_t* buf, size_t& len, size_t cap, uint8_t pin, uint8_t type, const void* val, uint8_t sz) {
    for (size_t p = 1; p + 2 <= len; ) {
        uint8_t t = buf[p + 1];
        size_t hdr = 2 + (RELINK_TYPE_SIZED(t) ? 1 : 0);
        size_t vs = t == RELINK_TYPE_INT16 ? 2 : RELINK_TYPE_SIZED(t) ? buf[p + 2] : 4;
        if (buf[p] == pin && t == type && vs == sz) { memcpy(buf + p + hdr, val, sz); return true; }
        p += hdr + vs;
    }
    size_t need = 2 + (RELINK_TYPE_SIZED(type) ? 1 : 0) + sz;
    if ((len ? len : 1) + need > cap || (len && buf[0] == 255)) return false;
    if (len == 0) buf[len++] = 0;   // count, patched as entries are added
    buf[len++] = pin;
    buf[len++] = type;
    if (RELINK_TYPE_SIZED(type)) buf[len++] = sz;
    memcpy(buf + len, val, sz); len += sz;
    buf[0]++;
    return true;
}

void RelinkIoT::_queueVPin(uint8_t pin, uint8_t type, const void* val, uint8_t sz) {
    if (batchPut(_txBuf, _txLen, sizeof(_txBuf), pin, type, val, sz)) return;
    // Full: send this chunk now (a full chunk can't wait for the pacer).
    flush();
    batchPut(_txBuf, _txLen, sizeof(_txBuf), pin, type, val, sz);
}

// Fold this batch into the relay's latest-value buffer.
void RelinkIoT::_relayMerge() {
    for (size_t p = 1; p + 2 <= _txLen; ) {
        uint8_t t = _txBuf[p + 1];
        size_t hdr = 2 + (RELINK_TYPE_SIZED(t) ? 1 : 0);
        uint8_t vs = t == RELINK_TYPE_INT16 ? 2 : RELINK_TYPE_SIZED(t) ? _txBuf[p + 2] : 4;
        if (!batchPut(_relayBuf, _relayLen, sizeof(_relayBuf), _txBuf[p], t, _txBuf + p + hdr, vs)) {
            _sendRelay();   // full: can't wait for the pacer
            batchPut(_relayBuf, _relayLen, sizeof(_relayBuf), _txBuf[p], t, _txBuf + p + hdr, vs);
        }
        p += hdr + vs;
    }
}

uint16_t RelinkIoT::_relayRate() {
#ifdef RELINK_HAS_WEBRTC
    if (_p2pHealthy() && (!_relayHz || _relayHz > RELINK_RELAY_HZ_P2P)) return RELINK_RELAY_HZ_P2P;
#endif
    return _relayHz;
}

void RelinkIoT::_sendRelay() {
    // Next slot on a fixed grid, so loop jitter doesn't push the average below the rate.
    uint16_t hz = _relayRate();
    unsigned long gap = hz ? 1000UL / hz : 0;
    _lastRelayTx = millis() - _lastRelayTx < 2 * gap ? _lastRelayTx + gap : millis();
    if (_relayLen == 0) return;
    size_t len = _relayLen;
    _relayLen = 0;
#ifdef RELINK_HAS_ENCRYPTION
    if (_encEnabled) {
        // Inner subtype 'D' = ENC_INNER_BULK: the same [count][entries] body
        // as 'B' without marker or CRC (the AEAD tag proves integrity).
        _sendEncrypted('D', _relayBuf, len);
        return;
    }
#endif
    if (_secure || _txBackoff()) return;
    // Plaintext: 'B'[count][entries...][crc32-LE]
    static uint8_t pkt[RELINK_TX_BUF + 5];
    pkt[0] = 'B';
    memcpy(pkt + 1, _relayBuf, len);
    uint32_t crc = _crc32(pkt, len + 1);
    memcpy(pkt + 1 + len, &crc, 4);
    _udp.beginPacket(_serverIp, _port);
    _udp.write(pkt, len + 5);
    if (!_udp.endPacket()) _txFailed(_serverIp, _port);
}

// The WiFi stack is out of buffers (endPacket errno 12): drop sends for a
// moment instead of retrying into a full queue every run(). Pins are latest-
// value, so the next batch after the pause carries whatever was lost.
void RelinkIoT::_txFailed(IPAddress ip, uint16_t port) {
    _txBackoffUntil = millis() + RELINK_TX_BACKOFF_MS;
    _txDrops++;
    _txFailedNow = true;
    _txInterval = _txInterval * 2 > RELINK_TX_MAX_MS ? RELINK_TX_MAX_MS : _txInterval * 2;
    _txOk = 0;
    if (millis() - _lastDropLog > 1000UL) {
        _lastDropLog = millis();
        _debug("[relink] WiFi TX full, %lu packets dropped (last to %s:%u)",
               _txDrops, ip.toString().c_str(), port);
    }
}

bool RelinkIoT::_txBackoff() {
    return (long)(millis() - _txBackoffUntil) < 0;
}

void RelinkIoT::flush() {
#ifdef RELINK_HAS_WEBRTC
    _flushP2P(false);
#endif
    if (_txLen == 0) return;
    _lastFlush = millis();
    // The previous batch went out without a full queue: speed back up.
    if (!_txFailedNow && ++_txOk >= 10 && _txInterval > RELINK_TX_MIN_MS) { _txInterval--; _txOk = 0; }
    _txFailedNow = false;
    if (!_registered || WiFi.status() != WL_CONNECTED || _secureBroken) { _txLen = 0; return; }
    bool relay = _txNeedsRelay;
#ifdef RELINK_HAS_ENCRYPTION
    if (_encEnabled) {
        relay = relay || !_meshAllUp();
        for (int i = 0; i < RELINK_MESH_PEERS; i++)
            if (_mesh[i].used && _mesh[i].up()) _sendEncryptedTo('D', _txBuf, _txLen, _mesh[i].addr, _mesh[i].addrPort);
    }
#endif
    if (relay) _relayMerge();
    _txLen = 0;
    _txNeedsRelay = false;
    if (_relayDue()) _sendRelay();
}

#ifdef RELINK_HAS_ENCRYPTION
// ── Encrypted transport ───────────────────────────────────────────────────────
void RelinkIoT::_loadPersistedNonce() {
    uint64_t persisted = 0;
#ifdef ESP32
    _encPrefs.begin("relinkenc", false);
    persisted = _encPrefs.getULong64("nonce", 0);
#else
    EEPROM.get(EEPROM_NONCE_ADDR, persisted);
    if (persisted == 0xFFFFFFFFFFFFFFFFULL) persisted = 0; // erased flash reads as all-1s
#endif
    // Jump past anything this device could plausibly have sent in a prior
    // session before an unclean reset (power loss, crash, watchdog) --
    // never resume exactly where we left off, since the last few increments
    // before a reset may never have been flushed to NVS/EEPROM.
    _nonceCounter = persisted + 256;
}

void RelinkIoT::_persistNonce() {
#ifdef ESP32
    _encPrefs.putULong64("nonce", _nonceCounter);
#else
    EEPROM.put(EEPROM_NONCE_ADDR, _nonceCounter);
    EEPROM.commit();
#endif
}

void RelinkIoT::enableEncryption(uint32_t deviceId, const char* keyHex) {
    if (!keyHex || strlen(keyHex) != 64) {
        _debug("[relink] enableEncryption: keyHex must be exactly 64 hex chars (32 bytes)");
        return;
    }
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < 32; i++) {
        int hi = hexval(keyHex[i * 2]), lo = hexval(keyHex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            _debug("[relink] enableEncryption: invalid hex character in key");
            return;
        }
        _encKey[i] = (uint8_t)((hi << 4) | lo);
    }
    _deviceId    = deviceId;
    _encEnabled  = true;
#ifdef ESP32
    _instance    = (uint16_t)(1 + esp_random() % 0xFFFF);
#else
    _instance    = (uint16_t)random(1, 0xFFFF);
#endif
    _loadPersistedNonce();
    _debug("[relink] encryption enabled, device_id=%lu, nonce resumes at %llu",
           (unsigned long)deviceId, (unsigned long long)_nonceCounter);
}

// Wire format: ['E'][device_id:4 LE][nonce:8 LE][ciphertext][tag:16]
// -- must match protocol.hpp / crypto_aead.hpp exactly.
void RelinkIoT::_sendEncrypted(uint8_t subtype, const uint8_t* inner, size_t innerLen) {
    _sendEncryptedTo(subtype, inner, innerLen, _serverIp, _port);
}

void RelinkIoT::_sendEncryptedTo(uint8_t subtype, const uint8_t* inner, size_t innerLen, IPAddress dstIp, uint16_t dstPort) {
    if (WiFi.status() != WL_CONNECTED) return;
    if (innerLen + 1 > RELINK_TX_BUF + 1) return; // guard against the fixed buffers below
    if (_txBackoff()) return;

    // [E][device_id:4][nonce:8][ciphertext][tag:16], the plaintext
    // ([subtype][inner]) encrypted in place in the packet -- ChaCha20 is a
    // stream cipher, so no separate plain/cipher buffers.
    static uint8_t pkt[1 + 4 + 8 + RELINK_TX_BUF + 1 + 16];
    uint8_t* body = pkt + 13;
    body[0] = subtype;
    memcpy(body + 1, inner, innerLen);
    size_t plainLen = innerLen + 1;

    uint64_t nonce = ((uint64_t)_instance << 48) | (++_nonceCounter & 0xFFFFFFFFFFFFULL);
    if ((_nonceCounter & 0xFF) == 0) {
        _persistNonce();
    }

    uint8_t iv[12];
    memcpy(iv, &_deviceId, 4);
    memcpy(iv + 4, &nonce, 8);

    ChaChaPoly ccp;
    ccp.setKey(_encKey, 32);
    ccp.setIV(iv, 12);
    ccp.encrypt(body, body, plainLen);

    pkt[0] = 'E';
    memcpy(pkt + 1, &_deviceId, 4);
    memcpy(pkt + 5, &nonce, 8);
    ccp.computeTag(body + plainLen, 16);
    size_t pos = 13 + plainLen + 16;

    _udp.beginPacket(dstIp, dstPort);
    _udp.write(pkt, pos);
    if (!_udp.endPacket()) _txFailed(dstIp, dstPort);
}

// ── Mesh ─────────────────────────────────────────────────────────────────────
// ['N'][count] x [instance:u16 LE][ip:4][port:2 BE][lan_ip:4][lan_port:2 BE]
void RelinkIoT::_meshPeers(const uint8_t* buf, int n) {
    bool seen[RELINK_MESH_PEERS] = {};
    int p = 2;
    for (uint8_t k = 0; k < buf[1] && p + 14 <= n; k++, p += 14) {
        uint16_t inst; memcpy(&inst, buf + p, 2);
        if (inst == _instance) continue;
        int slot = -1, freeSlot = -1;
        for (int i = 0; i < RELINK_MESH_PEERS; i++) {
            if (_mesh[i].used && _mesh[i].inst == inst) slot = i;
            else if (!_mesh[i].used && freeSlot < 0) freeSlot = i;
        }
        if (slot < 0) {
            if (freeSlot < 0) continue;   // more peers than slots: those stay on the relay
            slot = freeSlot;
            _mesh[slot] = MeshPeer();
            _mesh[slot].used = true;
            _mesh[slot].inst = inst;
            _debug("[relink] mesh peer %04x", inst);
        }
        MeshPeer& m = _mesh[slot];
        m.pub = IPAddress(buf[p + 2], buf[p + 3], buf[p + 4], buf[p + 5]);
        m.pubPort = ((uint16_t)buf[p + 6] << 8) | buf[p + 7];
        m.lan = IPAddress(buf[p + 8], buf[p + 9], buf[p + 10], buf[p + 11]);
        m.lanPort = ((uint16_t)buf[p + 12] << 8) | buf[p + 13];
        seen[slot] = true;
    }
    for (int i = 0; i < RELINK_MESH_PEERS; i++)
        if (_mesh[i].used && !seen[i]) _mesh[i].used = false;
    int t = 2 + buf[1] * 14;   // trailer: the relay's rate limit
    if (t + 2 <= n) _relayHz = buf[t] | (uint16_t)buf[t + 1] << 8;
}

void RelinkIoT::_meshPacket(const uint8_t* buf, int n, IPAddress ip, uint16_t port) {
    if (!_encEnabled || n < 13 + 16 + 1) return;
    uint32_t dev; uint64_t nonce;
    memcpy(&dev, buf + 1, 4); memcpy(&nonce, buf + 5, 8);
    if (dev != _deviceId) return;
    uint16_t inst = (uint16_t)(nonce >> 48);
    if (inst == _instance) return;
    int slot = -1;
    for (int i = 0; i < RELINK_MESH_PEERS; i++) if (_mesh[i].used && _mesh[i].inst == inst) slot = i;
    if (slot < 0) return;                       // only peers the relay vouched for
    MeshPeer& m = _mesh[slot];
    if (m.lastNonce && nonce <= m.lastNonce) return;   // replay / reorder

    static uint8_t plain[RELINK_RX_BUF];
    size_t cl = n - 13 - 16;
    if (cl > sizeof(plain)) return;
    uint8_t iv[12];
    memcpy(iv, buf + 1, 12);
    ChaChaPoly ccp;
    ccp.setKey(_encKey, 32);
    ccp.setIV(iv, 12);
    ccp.decrypt(plain, buf + 13, cl);
    if (!ccp.checkTag(buf + n - 16, 16)) return;

    m.lastNonce = nonce;
    bool wasUp = m.up();
    m.addr = ip; m.addrPort = port; m.lastRx = millis();
    if (!wasUp) {
        _debug("[relink] mesh peer %04x direct via %s:%u", inst, ip.toString().c_str(), port);
        uint8_t flags = _meshFlags();
        _sendEncryptedTo('H', &flags, 1, ip, port);
        _meshLinks(true);
    }
#ifdef RELINK_CAMERA_STREAM
    // Camera subscribe ['C'][on]: refreshed each second while the program wants frames.
    // Starting to watch (not a refresh) takes the stream over.
    if (plain[0] == 'C') {
        bool on = cl < 2 || plain[1];
        bool starting = on && !m.cam();
        m.camRx = on ? millis() : 0;
        if (starting) _camTake((int8_t)slot);
        else if (!on && _camOwner == slot) _camOwner = CAM_NONE;
        return;
    }
#endif
    if (plain[0] == 'D' && cl >= 2) {
        // [count] then [pin][dtype][value] -- the uplink body
        size_t p = 2;
        for (uint8_t k = 0; k < plain[1] && p + 2 <= cl; k++) {
            uint8_t t = plain[p + 1];
            size_t vs = t == RELINK_TYPE_INT16 ? 2 : (t == RELINK_TYPE_INT32 || t == RELINK_TYPE_FLOAT) ? 4
                      : (RELINK_TYPE_SIZED(t) && p + 3 <= cl) ? 1 + plain[p + 2] : 0;
            if (!vs || p + 2 + vs > cl) break;
            // Strings carry [len] before the bytes; handlers get the bytes only.
            const uint8_t* val = plain + p + 2 + (RELINK_TYPE_SIZED(t) ? 1 : 0);
            uint8_t vlen = (uint8_t)(RELINK_TYPE_SIZED(t) ? vs - 1 : vs);
#ifdef RELINK_HAS_WEBRTC
            if (!_p2pPinsKnown || _p2pPinAllowed(plain[p])) {
                _dispatchVPin(plain[p], t, val, vlen);
                // Gateway: a dashboard on our data channel sees peer writes too.
                if (_p2pHealthy() && _p2pPinsKnown) _queueP2P(plain[p], t, val, vlen);
            }
#else
            _dispatchVPin(plain[p], t, val, vlen);
#endif
            p += 2 + vs;
        }
    }
}

// 'L' to the relay: which instances we reach directly (it skips forwarding to them).
void RelinkIoT::_meshLinks(bool force) {
    uint8_t mask = 0;
    for (int i = 0; i < RELINK_MESH_PEERS; i++) if (_mesh[i].used && _mesh[i].up()) mask |= 1 << i;
    if (!force && mask == _lastLinkMask && millis() - _lastLinks < RELINK_REGISTER_MS) return;
    _lastLinkMask = mask; _lastLinks = millis();
    uint8_t m[1 + 2 * RELINK_MESH_PEERS];
    m[0] = 0;
    for (int i = 0; i < RELINK_MESH_PEERS; i++)
        if (mask & (1 << i)) { memcpy(m + 1 + 2 * m[0], &_mesh[i].inst, 2); m[0]++; }
    _sendEncrypted('L', m, 1 + 2 * m[0]);
}

// Every peer the relay told us about answers directly, so a relay copy of a
// gateway batch would reach nobody new.
bool RelinkIoT::_meshAllUp() const {
    for (int i = 0; i < RELINK_MESH_PEERS; i++) if (_mesh[i].used && !_mesh[i].up()) return false;
    return true;
}

// Hello flags. Bit 0: a P2P browser gets mesh writes through us (gateway), so
// peers that reach us directly can skip their relay copy.
void RelinkIoT::_sumAdd(uint8_t pin, uint8_t type, const void* val) {
    if (pin >= RELINK_MAX_VPINS) return;
    float v;
    if (type == RELINK_TYPE_INT16)      { int16_t x; memcpy(&x, val, 2); v = x; }
    else if (type == RELINK_TYPE_INT32) { int32_t x; memcpy(&x, val, 4); v = x; }
    else if (type == RELINK_TYPE_FLOAT) { memcpy(&v, val, 4); }
    else return;
    PinSum& s = _sum[pin];
    if (s.n == 0) { s.mn = s.mx = v; s.sum = 0; }
    if (v < s.mn) s.mn = v;
    if (v > s.mx) s.mx = v;
    s.sum += v;
    s.last = v;
    if (s.n < 0xFFFF) s.n++;
}

// 'M' = [count] then per pin [pin][n:u16][min][max][mean][last] (floats, LE),
// then [age_s:u32][seq:u16]. Queued, not sent: _sumFlush() sends the oldest
// until the relay acks it ('m'), so minutes without a relay aren't lost.
void RelinkIoT::_sendSummary() {
    _lastSummary = millis();
    if (!_encEnabled) return;
    static uint8_t buf[1 + RELINK_MAX_VPINS * 19];
    size_t len = 1;
    buf[0] = 0;
    for (unsigned p = 0; p < RELINK_MAX_VPINS && buf[0] < 255; p++) {
        PinSum& s = _sum[p];
        if (!s.n) continue;
        float mean = s.sum / s.n;
        buf[len++] = p;
        memcpy(buf + len, &s.n, 2);    len += 2;
        memcpy(buf + len, &s.mn, 4);   len += 4;
        memcpy(buf + len, &s.mx, 4);   len += 4;
        memcpy(buf + len, &mean, 4);   len += 4;
        memcpy(buf + len, &s.last, 4); len += 4;
        buf[0]++;
        s.n = 0;
    }
    if (!buf[0]) return;
    size_t rec = 8 + len;
    if (rec > sizeof(_sumBuf)) return;
    while (_sumBufLen + rec > sizeof(_sumBuf)) {          // full: drop the oldest
        uint16_t l; memcpy(&l, _sumBuf, 2);
        memmove(_sumBuf, _sumBuf + l, _sumBufLen - l);
        _sumBufLen -= l;
        _sumInFlight = false;
    }
    uint8_t* r = _sumBuf + _sumBufLen;
    uint16_t l = rec, seq = _sumSeq++;
    uint32_t t = millis();
    memcpy(r, &l, 2); memcpy(r + 2, &seq, 2); memcpy(r + 4, &t, 4);
    memcpy(r + 8, buf, len);
    _sumBufLen += rec;
}

void RelinkIoT::_sumFlush() {
    if (!_sumBufLen || !_registered) return;
    if (_sumInFlight && millis() - _sumSentAt < RELINK_SUMMARY_RETRY_MS) return;
    uint16_t l, seq; uint32_t t;
    memcpy(&l, _sumBuf, 2); memcpy(&seq, _sumBuf + 2, 2); memcpy(&t, _sumBuf + 4, 4);
    static uint8_t pkt[1 + RELINK_MAX_VPINS * 19 + 6];
    size_t body = l - 8;
    uint32_t age = (millis() - t) / 1000;
    memcpy(pkt, _sumBuf + 8, body);
    memcpy(pkt + body, &age, 4);
    memcpy(pkt + body + 4, &seq, 2);
    _sendEncrypted('M', pkt, body + 6);
    _sumInFlight = true;
    _sumSentAt = millis();
}

void RelinkIoT::_sumAcked(uint16_t seq) {
    if (!_sumBufLen) return;
    uint16_t l, head; memcpy(&l, _sumBuf, 2); memcpy(&head, _sumBuf + 2, 2);
    if (head != seq) return;
    memmove(_sumBuf, _sumBuf + l, _sumBufLen - l);
    _sumBufLen -= l;
    _sumInFlight = false;   // next one goes out on the next run()
}

uint8_t RelinkIoT::_meshFlags() {
#ifdef RELINK_HAS_WEBRTC
    if (_p2pHealthy() && _p2pPinsKnown) return 1;
#endif
    return 0;
}

void RelinkIoT::_meshTick() {
    if (!_encEnabled || !_registered) return;
    uint8_t flags = _meshFlags();
    bool changed = flags != _lastMeshFlags;   // tell peers now, not at the next hello
    _lastMeshFlags = flags;
    for (int i = 0; i < RELINK_MESH_PEERS; i++) {
        MeshPeer& m = _mesh[i];
        if (!m.used || (!changed && millis() - m.lastHello < 1000UL)) continue;
        m.lastHello = millis();
        if (m.up()) _sendEncryptedTo('H', &flags, 1, m.addr, m.addrPort);
        else {
            if (m.pubPort) _sendEncryptedTo('H', &flags, 1, m.pub, m.pubPort);
            if (m.lanPort && (m.lan != m.pub || m.lanPort != m.pubPort)) _sendEncryptedTo('H', &flags, 1, m.lan, m.lanPort);
        }
    }
    _meshLinks(false);
}
#endif

// ── Internal: poll UDP recv ───────────────────────────────────────────────────
void RelinkIoT::_pollRecv() {
    // Drain what's queued (bounded, so a flood can't starve loop()) -- one
    // packet per run() fell behind as soon as writes arrived faster than loop().
    for (int i = 0; i < 16; i++) {
        int pktSize = _udp.parsePacket();
        if (pktSize <= 0) return;
        int n = _udp.read(_rxBuf, sizeof(_rxBuf) - 1);
        if (n <= 0) continue;
#ifdef RELINK_HAS_ENCRYPTION
        if (_rxBuf[0] == 'E') { _meshPacket(_rxBuf, n, _udp.remoteIP(), _udp.remotePort()); continue; }
#endif
        _processFrame(_rxBuf, n);
    }
}

// ── Internal: parse server frames ────────────────────────────────────────────
void RelinkIoT::_processFrame(const uint8_t* buf, int n) {
    if (n < 1) return;

    if (buf[0] == '#') {
        // Registration ACK (pin 0x0F = internal) or VPin frame
        if (n >= 2 && buf[1] == 0x0F && !_registered) {
            _registered = true;
            _debug("[relink] registered (server ACK)");
            if (_onRegisteredCb) _onRegisteredCb();
        } else if (n >= 3) {
            _dispatchVPin(buf[1], buf[2], buf + 3, (uint8_t)(n - 3));
        }
        return;
    }

    if (buf[0] == '@') {
        // Heartbeat
        _lastHeartbeat = millis();
        return;
    }

#ifdef RELINK_HAS_ENCRYPTION
    if (buf[0] == 'N' && n >= 2) { _meshPeers(buf, n); return; }
    if (buf[0] == 'm' && n == 3) { uint16_t q; memcpy(&q, buf + 1, 2); _sumAcked(q); return; }
    if (buf[0] == 'G' && n == 1) {
        // Relay lost us (restart / expiry): register within RELINK_REJOIN_SPREAD_MS
        // rather than at the next heartbeat. Not at once: after a relay restart
        // every device gets this 'G' together.
        _lastReg = millis() - RELINK_REGISTER_MS + (unsigned long)random(0, RELINK_REJOIN_SPREAD_MS);
        _meshLinksDue = true;
        return;
    }
#endif

    if (buf[0] == 'O' && n == 1) {
        _otaWake = true;   // new firmware uploaded: check on the next run()
        return;
    }

#ifndef RELINK_NO_OTA
    if (buf[0] == 'V' && n >= 2) {
        // Relay's answer to our 'U' report: "ver=<n>&fs=<bytes>".
        char txt[48];
        size_t tl = min((size_t)n - 1, sizeof(txt) - 1);
        memcpy(txt, buf + 1, tl); txt[tl] = 0;
        const char* pv = strstr(txt, "ver=");
        const char* pf = strstr(txt, "fs=");
        int32_t ver = pv ? atol(pv + 4) : 0;
        _otaReported = true;
        _otaError = "";       // the report carrying it got through
        if (_otaEnabled && ver > getFirmwareVersion() && ver != _otaFailedVer) {
            _otaPendingVer = ver;
            _otaPendingFs  = pf ? atol(pf + 3) : 0;
        }
        return;
    }
#endif

    if (buf[0] == 'W' && n == 1) {
#ifdef RELINK_HAS_WEBRTC
        _rtcWake = true;   // a dashboard posted a P2P offer
#endif
        return;
    }

    if (buf[0] == 'M' && n >= 2) {
        // Batched browser writes: ['M'][count] then [pin][dtype][len][value:len]
        int p = 2;
        for (uint8_t i = 0; i < buf[1] && p + 3 <= n; i++) {
            uint8_t len = buf[p + 2];
            if (p + 3 + len > n) break;
            _dispatchVPin(buf[p], buf[p + 1], buf + p + 3, len);
            p += 3 + len;
        }
        return;
    }

    if (buf[0] == 'S' && n >= 3) {
        // SET_PIN relay: browser → server → device
        _dispatchVPin(buf[1], buf[2], buf + 3, (uint8_t)(n - 3));
        return;
    }
}

// ── Internal: dispatch to callbacks ──────────────────────────────────────────
void RelinkIoT::_dispatchVPin(uint8_t pin, uint8_t type, const uint8_t* val, uint8_t vsz) {

    // Typed lambda callbacks
    if (pin < RELINK_MAX_VPINS && _vpinHandlers[pin].type != 255) {
        auto& h = _vpinHandlers[pin];
        if (type == RELINK_TYPE_INT16 && vsz >= 2 && h.cbInt16) {
            int16_t v; memcpy(&v, val, 2); h.cbInt16(v);
        } else if (type == RELINK_TYPE_INT32 && vsz >= 4 && h.cbInt32) {
            int32_t v; memcpy(&v, val, 4); h.cbInt32(v);
        } else if (type == RELINK_TYPE_FLOAT && vsz >= 4 && h.cbFloat) {
            float v; memcpy(&v, val, 4); h.cbFloat(v);
        } else if (type == RELINK_TYPE_STRING && h.cbString) {
            h.cbString((const char*)val, vsz);
        } else if (type == RELINK_TYPE_BYTES && h.cbBytes) {
            h.cbBytes(val, vsz);
        }
    }

    // RELINK_WRITE macro handlers
    for (uint8_t i = 0; i < relinkMacroHandlerCount; i++) {
        if (relinkMacroHandlers[i].pin != pin || !relinkMacroHandlers[i].handler) continue;
        if (type == RELINK_TYPE_INT16 && vsz >= 2) {
            int16_t v; memcpy(&v, val, 2);
            relinkMacroHandlers[i].handler(RelinkParam(v));
        } else if (type == RELINK_TYPE_INT32 && vsz >= 4) {
            int32_t v; memcpy(&v, val, 4);
            relinkMacroHandlers[i].handler(RelinkParam(v));
        } else if (type == RELINK_TYPE_FLOAT && vsz >= 4) {
            float v; memcpy(&v, val, 4);
            relinkMacroHandlers[i].handler(RelinkParam(v));
        } else if (type == RELINK_TYPE_BYTES) {
            relinkMacroHandlers[i].handler(RelinkParam(val, vsz));
        } else if (type == RELINK_TYPE_STRING) {
            relinkMacroHandlers[i].handler(RelinkParam((const char*)val, vsz));
        }
    }
}

// ── VPin callbacks ────────────────────────────────────────────────────────────
void RelinkIoT::onVirtualPin(uint8_t p, RelinkCallbackInt16  cb) { if (p < RELINK_MAX_VPINS) { _vpinHandlers[p].type = RELINK_TYPE_INT16;  _vpinHandlers[p].cbInt16  = cb; } }
void RelinkIoT::onVirtualPin(uint8_t p, RelinkCallbackInt32  cb) { if (p < RELINK_MAX_VPINS) { _vpinHandlers[p].type = RELINK_TYPE_INT32;  _vpinHandlers[p].cbInt32  = cb; } }
void RelinkIoT::onVirtualPin(uint8_t p, RelinkCallbackFloat  cb) { if (p < RELINK_MAX_VPINS) { _vpinHandlers[p].type = RELINK_TYPE_FLOAT;  _vpinHandlers[p].cbFloat  = cb; } }
void RelinkIoT::onVirtualPin(uint8_t p, RelinkCallbackString cb) { if (p < RELINK_MAX_VPINS) { _vpinHandlers[p].type = RELINK_TYPE_STRING; _vpinHandlers[p].cbString = cb; } }
void RelinkIoT::onVirtualPin(uint8_t p, RelinkCallbackBytes  cb) { if (p < RELINK_MAX_VPINS) { _vpinHandlers[p].type = RELINK_TYPE_BYTES;  _vpinHandlers[p].cbBytes  = cb; } }

// ── Events ────────────────────────────────────────────────────────────────────
void RelinkIoT::onConnect   (RelinkCallback cb) { _onConnectCb    = cb; }
void RelinkIoT::onDisconnect(RelinkCallback cb) { _onDisconnectCb = cb; }
void RelinkIoT::onRegistered(RelinkCallback cb) { _onRegisteredCb = cb; }

// ── State ─────────────────────────────────────────────────────────────────────
bool RelinkIoT::connected()  { return WiFi.status() == WL_CONNECTED; }
bool RelinkIoT::registered() { return _registered; }

void     RelinkIoT::setDebug(bool e) { _debugEnabled = e; }
uint32_t RelinkIoT::getUptime()      { return (millis() - _startTime) / 1000UL; }

int32_t RelinkIoT::getFirmwareVersion() {
    int32_t v = 0;
    EEPROM.get(EEPROM_FW_ADDR, v);
    return v;
}

// ── OTA update ────────────────────────────────────────────────────────────────
#ifndef RELINK_NO_OTA
void RelinkIoT::_otaSend(const String& msg) {
#ifdef RELINK_HAS_ENCRYPTION
    if (_encEnabled) { _sendEncrypted('U', (const uint8_t*)msg.c_str(), msg.length()); return; }
#endif
    _udp.beginPacket(_serverIp, _port);
    _udp.write('U');
    _udp.write((const uint8_t*)msg.c_str(), msg.length());
    _udp.endPacket();
}

// Report OTA state to the relay ('U'); its 'V' reply says whether to update.
// Also carries the running version (how the dashboard confirms an install
// after the reboot), flash usage, and once, why the last attempt failed.
void RelinkIoT::_checkOTA() {
    if (!_token) return;
    // Sketch size is fixed until the next reboot (and slow-ish to compute on
    // ESP32), so once.
    static uint32_t sketchSize = ESP.getSketchSize();
    String m = String("ota=") + (_otaEnabled ? "1" : "0")
             + "&ver=" + String((long)getFirmwareVersion())
             + "&sk=" + String(sketchSize) + "&slot=" + String(ESP.getFreeSketchSpace())
             + "&flash=" + String(ESP.getFlashChipSize()) + "&heap=" + String(ESP.getFreeHeap());
    if (_otaError.length()) m += "&err=" + _otaError;
    _otaSend(m);
}

void RelinkIoT::_applyOTA(int32_t serverVer, long fsSize) {
    if (!_otaApiBase) return;
    _debug("[OTA] new firmware v%d (local v%d)", (int)serverVer, (int)getFirmwareVersion());

    // A "fw+fs" package also carries a LittleFS image. It goes first: the
    // firmware write is what switches the boot partition, so if the
    // filesystem fails the device keeps running the old firmware and retries
    // next poll instead of booting new code against an old filesystem.
    if (fsSize > 0) {
        _debug("[OTA] writing filesystem (%ld bytes)", fsSize);
#ifdef ESP32
        if (!_otaFlash("littlefs", U_SPIFFS)) { _otaFailedVer = serverVer; _checkOTA(); return; }
#else
        if (!_otaFlash("littlefs", U_FS)) { _otaFailedVer = serverVer; _checkOTA(); return; }
#endif
    }
    // On failure, report the error right away (_checkOTA carries _otaError).
    if (!_otaFlash("firmware", U_FLASH)) { _otaFailedVer = serverVer; _checkOTA(); return; }

    _debug("[OTA] success — rebooting");
    // Save new version to EEPROM before restart
    EEPROM.put(EEPROM_FW_ADDR, serverVer);
    EEPROM.commit();
    delay(500);
    ESP.restart();
}

// Bytes really written to flash, for the dashboard's progress bar: one
// fire-and-forget UDP datagram, so the download never waits on it.
void RelinkIoT::_otaReportProgress(const char* part, int written, int total) {
    _otaSend(String("p=") + part + "&w=" + written + "&t=" + total);
}

// Streams one image (?part=firmware|littlefs) from the server into flash.
bool RelinkIoT::_otaFlash(const char* part, int command) {
    String dlUrl = String(_otaApiBase) + "/api/firmware/download/" + _token + "?part=" + part;

    HTTPClient http;
    WiFiClient dlClient;
    http.begin(dlClient, dlUrl);

    int code = http.GET();
    if (code != 200) {
        _debug("[OTA] %s download HTTP %d", part, code);
        _otaError = String(part) + "_http_" + code;
        http.end(); return false;
    }

    int totalLen = http.getSize();
    WiFiClient* stream = http.getStreamPtr();

#ifdef ESP32
    // ESP32's Updater accepts an unknown size and figures out space as it
    // streams in.
    bool updateStarted = Update.begin(totalLen > 0 ? totalLen : UPDATE_SIZE_UNKNOWN, command);
#else
    // ESP8266's Updater has no "unknown size" sentinel -- begin() needs the
    // real byte count up front to compute the flash region, so a response
    // without Content-Length can't be flashed here.
    if (totalLen <= 0) {
        _debug("[OTA] server did not report a Content-Length; ESP8266 OTA requires a known size");
        _otaError = String(part) + "_no_length";
        http.end();
        return false;
    }
    bool updateStarted = Update.begin((size_t)totalLen, command);
#endif
    if (!updateStarted) {
        _debug("[OTA] %s: Update.begin failed (image larger than its partition?)", part);
        _otaError = String(part) + "_too_big_or_no_partition";
        http.end();
        return false;
    }

    static uint8_t buf[1460];   // one TCP segment per read; static keeps it off the stack
    int written = 0;
    int lastPct = 0;
    while (http.connected() && (totalLen < 0 || written < totalLen)) {
        int avail = stream->available();
        if (!avail) { delay(1); continue; }
        int rd = stream->readBytes(buf, min((int)sizeof(buf), avail));
        if (rd > 0) {
            Update.write(buf, rd);
            written += rd;
        }
        // Report bytes really written to flash every 2% for the dashboard's
        // progress bar.
        int pct = totalLen > 0 ? (int)((int64_t)written * 100 / totalLen) : 0;
        if (pct >= lastPct + 2 && written < totalLen) {
            lastPct = pct;
            _otaReportProgress(part, written, totalLen);
        }
    }
    http.end();
    if (totalLen > 0) _otaReportProgress(part, written, totalLen);

    if (!Update.end(true)) {
        _debug("[OTA] %s: Update.end failed (error %d)", part, (int)Update.getError());
        _otaError = String(part) + "_write_failed_" + (int)Update.getError();
        return false;
    }
    return true;
}
#else
void RelinkIoT::_checkOTA() {}
#endif

// ── Secure bootstrap (claim code) ────────────────────────────────────────────
#ifndef RELINK_NO_OTA
bool RelinkIoT::claim(const char* claimCode, const char* apiBase,
                       char* tokenOut, size_t tokenOutLen,
                       char* keyHexOut, size_t keyHexOutLen,
                       uint32_t* checksumOut) {
    if (!claimCode || !apiBase || !tokenOut || !keyHexOut || !checksumOut) return false;
    if (WiFi.status() != WL_CONNECTED) {
        _debug("[claim] WiFi not connected");
        return false;
    }

    String url = String(apiBase) + "/api/device/claim";
    HTTPClient http;
    WiFiClient wc;
    http.begin(wc, url);
    http.addHeader("Content-Type", "application/json");

    String body = String("{\"claim_code\":\"") + claimCode + "\"}";
    int code = http.POST(body);
    if (code != 200) {
        _debug("[claim] server returned HTTP %d (expired/already-used code?)", code);
        http.end();
        return false;
    }
    String resp = http.getString();
    http.end();

    // Response shape is fixed ({"link_token":"...","enc_key_hex":"...",
    // "checksum":N}), so a small strstr-based extractor (same approach
    // _checkOTA already uses for its JSON) is enough -- no parser dependency.
    auto extractQuoted = [&](const char* key) -> String {
        int idx = resp.indexOf(key);
        if (idx < 0) return String();
        int q1 = resp.indexOf('"', idx + strlen(key));
        int q2 = (q1 >= 0) ? resp.indexOf('"', q1 + 1) : -1;
        if (q1 < 0 || q2 < 0) return String();
        return resp.substring(q1 + 1, q2);
    };

    String token  = extractQuoted("\"link_token\"");
    String keyHex = extractQuoted("\"enc_key_hex\"");
    int cIdx = resp.indexOf("\"checksum\"");
    if (token.length() == 0 || keyHex.length() == 0 || cIdx < 0) {
        _debug("[claim] malformed response from server");
        return false;
    }
    int colon = resp.indexOf(':', cIdx);
    uint32_t checksum = (uint32_t)strtoul(resp.c_str() + colon + 1, nullptr, 10);

    if (token.length() + 1 > tokenOutLen || keyHex.length() + 1 > keyHexOutLen) {
        _debug("[claim] output buffer too small");
        return false;
    }
    strncpy(tokenOut, token.c_str(), tokenOutLen);
    strncpy(keyHexOut, keyHex.c_str(), keyHexOutLen);
    *checksumOut = checksum;
    _debug("[claim] success, checksum=%lu", (unsigned long)checksum);
    return true;
}
#endif

// ── WebRTC-lite data channel ──────────────────────────────────────────────────
#ifdef RELINK_HAS_WEBRTC
#include <lwip/sockets.h>
#include <fcntl.h>

static uint32_t _relinkWebrtcNowMs() { return millis(); }

void RelinkIoT::_webrtcOnOpenTrampoline(void* user) {
    RelinkIoT* self = (RelinkIoT*)user;
    if (self->_webrtcOnOpenCb) self->_webrtcOnOpenCb();
}

void RelinkIoT::_webrtcOnMessageTrampoline(void* user, const uint8_t* data, size_t len) {
    RelinkIoT* self = (RelinkIoT*)user;
    // Browser SET_PIN (webapp protocol.ts C2S.SET_PIN): [0x02][pin][dtype][value...]
    // Dispatched to the same RELINK_WRITE / onVirtualWrite handlers as a
    // relayed write -- but only for declared datastream pins, since nothing
    // on the server sees this channel.
    if (len >= 3 && data[0] == 0x02) {
        if (self->_rtcReadOnly) {
            self->_debug("[webrtc] dropped P2P write to V%u (view-only share)", (unsigned)data[1]);
            return;
        }
        if (!self->_p2pPinAllowed(data[1])) {
            self->_debug("[webrtc] dropped P2P write to V%u (not a datastream)", (unsigned)data[1]);
            return;
        }
        size_t vsz = len - 3;
        self->_dispatchVPin(data[1], data[2], data + 3, (uint8_t)(vsz > 255 ? 255 : vsz));
#ifdef RELINK_HAS_ENCRYPTION
        // The relay never sees this write: count it in our own summary ('M').
        if (vsz >= (data[2] == RELINK_TYPE_INT16 ? 2u : 4u)) self->_sumAdd(data[1], data[2], data + 3);
        // Gateway: pass the browser's write on to mesh peers (and the relay
        // for any peer we don't reach directly), never back to the browser.
        if (self->_encEnabled) self->_queueVPin(data[1], data[2], data + 3, (uint8_t)(vsz > 255 ? 255 : vsz));
#endif
    }
#ifdef RELINK_CAMERA_STREAM
    // Camera widget on screen (1) / gone (0): [0x12][on]
    if (len >= 2 && data[0] == 0x12) {
        bool on = data[1] != 0;
        if (on && !self->_camBrowser) self->_camTake(CAM_BROWSER);   // started watching: takes over
        else if (!on && self->_camOwner == CAM_BROWSER) self->_camOwner = CAM_NONE;
        self->_camBrowser = on;
    }
#endif
    if (self->_webrtcOnMessageCb) self->_webrtcOnMessageCb(data, len);
}

void RelinkIoT::_p2pParsePins(const char* csv) {
    memset(_p2pPinMask, 0, sizeof(_p2pPinMask));
    for (const char* p = csv; *p; ) {
        char* end;
        long v = strtol(p, &end, 10);
        if (end == p) { p++; continue; }
        if (v >= 0 && v < 256) _p2pPinMask[v >> 3] |= (uint8_t)(1u << (v & 7));
        p = end;
    }
    _p2pPinsKnown   = true;
    _p2pPinsFetched = millis();
}

// Each UDP send costs the ESP32 ~2 ms (lwIP hands it to its own task and
// waits), whatever its size, so the data channel gets few, full packets:
// sent once it's ~60% full or has waited RELINK_P2P_MAX_WAIT_MS. Nothing is
// dropped by waiting -- this buffer appends every sample, it never merges.
#ifndef RELINK_P2P_MAX_WAIT_MS
#define RELINK_P2P_MAX_WAIT_MS 5
#endif
// Largest data-channel message that fits one DTLS record: the session's MTU
// is 1200 B and SCTP adds 28 B, DTLS 37-77 B depending on the cipher. A
// bigger message is refused by mbedTLS -- it used to fill up to 1200 B and
// vanish (~48% loss once forwarded mesh pins filled the buffer).
#ifndef RELINK_P2P_MAX_PACKET
#define RELINK_P2P_MAX_PACKET 1080
#endif
void RelinkIoT::_flushP2P(bool force) {
    if (!_p2pLen) return;
    if (!force && _p2pLen < RELINK_P2P_MAX_PACKET * 6 / 10 && millis() - _lastP2PSend < RELINK_P2P_MAX_WAIT_MS) return;
    _p2pBuf[0] = 102;   // S2C.PIN_BATCH
    if (_p2pHealthy()) webrtcSend(_p2pBuf, _p2pLen);
    _p2pLen = 0;
    _lastP2PSend = millis();
}

void RelinkIoT::_queueP2P(uint8_t pin, uint8_t type, const void* val, uint8_t sz) {
    if (_p2pLen + 3 + sz > RELINK_P2P_MAX_PACKET || (_p2pLen && _p2pBuf[1] == 255)) _flushP2P(true);
    if (_p2pLen == 0) { _p2pBuf[0] = 102; _p2pBuf[1] = 0; _p2pLen = 2; }
    _p2pBuf[_p2pLen++] = pin;
    _p2pBuf[_p2pLen++] = type;
    _p2pBuf[_p2pLen++] = sz;
    memcpy(_p2pBuf + _p2pLen, val, sz); _p2pLen += sz;
    _p2pBuf[1]++;
}

// P2P runs as a series of sessions, one per dashboard: a new offer replaces
// whatever session exists (fresh DTLS identity included), a session that
// fails its handshake or goes silent is closed, and VPin traffic falls back
// to the relay in between. webrtcBegin() just turns that on.
bool RelinkIoT::webrtcBegin(uint16_t udpPort, const char* local_ice_pwd, const char* apiBase) {
    _rtcAutoTried = true;
    _rtcUdpPort   = udpPort;
    _rtcIcePwd    = local_ice_pwd;
    _webrtcApiBase = apiBase;
    _rtcEnabled   = true;
    _rtcWake      = true;    // check once now: a dashboard may already be waiting
    _lastWebrtcSignalCheck = millis();
    if (_rtcActive) return true;
    return _rtcOpenSession();
}

bool RelinkIoT::_rtcOpenSession() {
    // Fresh ICE password per session: an older dashboard's consent checks
    // would otherwise still validate and could claim this session's peer.
    if (_rtcIcePwd == _rtcAutoPwd)
        for (int i = 0; i < 32; i++) _rtcAutoPwd[i] = "abcdefghijklmnopqrstuvwxyz0123456789"[esp_random() % 36];
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { _debug("[webrtc] socket() failed"); return false; }
    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(_rtcUdpPort);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        _debug("[webrtc] bind() failed on port %u", (unsigned)_rtcUdpPort);
        close(fd);
        return false;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
#ifdef RELINK_CAMERA_STREAM
    if (!_rtcMu) _rtcMu = xSemaphoreCreateRecursiveMutex();
#endif
    RTC_LOCK();
    int rc = rtc_session_init(&_rtcSession, fd, _rtcIcePwd, _relinkWebrtcNowMs,
                               _webrtcOnOpenTrampoline, _webrtcOnMessageTrampoline, this);
    RTC_UNLOCK();
    if (rc != 0) {
        _debug("[webrtc] rtc_session_init failed: %d", rc);
        close(fd);
        return false;
    }
    _rtcSockFd    = fd;
    _rtcActive    = true;
    _rtcAnswered  = false;
    _rtcBeginTime = millis();
    _debug("[webrtc] session ready on UDP :%u", (unsigned)_rtcUdpPort);
    return true;
}

void RelinkIoT::_rtcCloseSession(const char* why) {
#ifdef RELINK_CAMERA_STREAM
    _camBrowser = false;
    if (_camOwner == CAM_BROWSER) _camOwner = CAM_NONE;
#endif
    if (!_rtcActive) return;
    _debug("[webrtc] closing P2P session (%s) -- VPin traffic back on the relay", why);
    RTC_LOCK();
    dtls_lite_deinit(&_rtcSession.dtls);
    if (_rtcSockFd >= 0) close(_rtcSockFd);
    _rtcSockFd   = -1;
    _rtcActive   = false;
    RTC_UNLOCK();
    _rtcAnswered = false;
    _p2pLen      = 0;
}

#include "stun_lite.h"

#ifndef RELINK_STUN_PORT
#define RELINK_STUN_PORT 3478   // relink_sig's STUN responder on the Relink server
#endif

// Hole punching. Over the internet the browser's checks reach us only if our
// NAT has a mapping toward the browser: learn our public address (srflx) from
// the server's STUN, put it in the answer, then send a few packets to the
// browser's own srflx candidates so the NAT lets its checks in.
struct _RtcPunchAddr { uint32_t ip; uint16_t port; };   // host byte order
static _RtcPunchAddr _rtcPunch[4];
static int _rtcPunchN = 0;
static unsigned long _rtcLastPunch = 0;

// Public ip:port of fd as STUN server:sport sees it; false if no answer.
static bool _rtcStunDiscover2(int fd, const char* server, uint16_t sport, uint32_t* ip, uint16_t* port) {
    IPAddress srv;
    if (!server || !WiFi.hostByName(server, srv)) return false;
    struct sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(sport);
    to.sin_addr.s_addr = (uint32_t)srv;
    uint8_t tid[12], req[20], buf[256];
    for (int i = 0; i < 12; i++) tid[i] = (uint8_t)esp_random();
    stun_lite_build_binding_request(tid, req);
    for (int attempt = 0; attempt < 3; attempt++) {
        sendto(fd, req, sizeof(req), 0, (struct sockaddr*)&to, sizeof(to));
        unsigned long t0 = millis();
        while (millis() - t0 < 250) {
            int n = recvfrom(fd, buf, sizeof(buf), 0, nullptr, nullptr);
            if (n > 0 && stun_lite_parse_binding_success(buf, (size_t)n, tid, ip, port)) return true;
            if (n <= 0) delay(5);
        }
    }
    return false;
}

// The browser's public (srflx / relay) UDP candidates from its offer, for punching.
static void _rtcParseOfferCandidates(const char* sdp) {
    _rtcPunchN = 0;
    for (const char* p = strstr(sdp, "a=candidate:"); p && _rtcPunchN < 4; p = strstr(p + 1, "a=candidate:")) {
        char proto[8], ipStr[48], typ[8];
        unsigned port;
        if (sscanf(p, "a=candidate:%*s %*u %7s %*u %47s %u typ %7s", proto, ipStr, &port, typ) != 4) continue;
        // srflx: the browser's own NAT; relay: its TURN address (the Relink
        // server), the one that still works when both NATs are strict.
        if (strcasecmp(proto, "udp") != 0 || (strcmp(typ, "srflx") != 0 && strcmp(typ, "relay") != 0)) continue;
        IPAddress a;
        if (!a.fromString(ipStr)) continue;   // IPv6 / mDNS names
        _rtcPunch[_rtcPunchN].ip = ntohl((uint32_t)a);
        _rtcPunch[_rtcPunchN].port = (uint16_t)port;
        _rtcPunchN++;
    }
}


void RelinkIoT::webrtcPoll() {
    if (!_rtcEnabled || !_netUp) return;
    if (_rtcActive) { RTC_LOCK(); rtc_session_poll(&_rtcSession); RTC_UNLOCK(); }
    unsigned long now = millis();

    // Look for an offer: right away when the relay nudged us, otherwise a
    // slow fallback poll (the HTTP request blocks loop() briefly).
    if (_webrtcApiBase && (_rtcWake || now - _lastWebrtcSignalCheck > RELINK_P2P_FALLBACK_POLL_MS)) {
        _rtcWake = false;
        _lastWebrtcSignalCheck = now;
        _webrtcCheckSignaling();
        // The HTTP round trips took a while and may have set _rtcAnswerTime
        // after `now`: an unsigned now - _rtcAnswerTime would wrap and close
        // the session the moment it was answered.
        now = millis();
    }
    if (!_rtcActive) return;

    // Until the browser's first check gets through, keep our NAT open toward it.
    if (_rtcAnswered && !_rtcSession.peer_known && _rtcPunchN > 0 &&
        (uint32_t)now - (uint32_t)_rtcAnswerTime < 8000UL && now - _rtcLastPunch >= 200UL) {
        _rtcLastPunch = now;
        uint8_t tid[12], req[20];
        for (int i = 0; i < 12; i++) tid[i] = (uint8_t)esp_random();
        stun_lite_build_binding_request(tid, req);
        for (int i = 0; i < _rtcPunchN; i++) {
            struct sockaddr_in to = {};
            to.sin_family = AF_INET;
            to.sin_port = htons(_rtcPunch[i].port);
            to.sin_addr.s_addr = htonl(_rtcPunch[i].ip);
            sendto(_rtcSockFd, req, sizeof(req), 0, (struct sockaddr*)&to, sizeof(to));
        }
    }

    if (_rtcSession.state == RTC_SESSION_READY) {
        if ((uint32_t)now - _rtcSession.last_rx_ms > RELINK_P2P_IDLE_MS) { _rtcCloseSession("dashboard gone"); return; }
        // Keep-alive [103] when nothing else went out for a second, so the
        // browser can tell a quiet device from a dead link.
        if (now - _lastP2PSend >= 1000UL) {
            static const uint8_t ka = 103;
            webrtcSend(&ka, 1);
            _lastP2PSend = now;
        }
        if (_webrtcApiBase && now - _p2pPinsFetched > RELINK_P2P_PINS_REFRESH_MS) {
            _p2pPinsFetched = now;   // also rate-limits retries on failure
            _webrtcRefreshPins();
        }
    } else if (_rtcAnswered && (uint32_t)now - max((uint32_t)_rtcAnswerTime, _rtcSession.last_rx_ms) > RELINK_WEBRTC_TIMEOUT_MS) {
        // Idle, not a fixed deadline: a handshake still exchanging packets
        // (the browser keeps sending ICE checks) is left to finish.
        _rtcCloseSession("handshake timed out (nothing from the browser)");
    } else if (_rtcSession.state == RTC_SESSION_FAILED) {
        _rtcCloseSession("handshake failed");
    }
}

// ── One-shot offer/answer signaling against api/app.py's
// /api/device/webrtc-{offer,answer}/{token} (see that file for the server
// side). Polls for a pending offer every ~1s (from webrtcPoll() above)
// until it finds one, builds and posts an answer exactly once, then stops
// -- the actual connectivity check/DTLS/SCTP handshake that follows runs
// entirely over the UDP socket already bound in webrtcBegin(), independent
// of this HTTP exchange.
static bool _webrtcJsonUnescapeField(const char* json, const char* key, char* out, size_t outCap) {
    String search = String("\"") + key + "\":";
    int idx = String(json).indexOf(search);
    if (idx < 0) return false;
    int p = idx + search.length();
    if (json[p] != '"') return false; // null or malformed -- nothing to unescape
    p++;
    size_t o = 0;
    while (json[p] && json[p] != '"' && o + 1 < outCap) {
        if (json[p] == '\\' && json[p + 1]) {
            p++;
            char esc = json[p];
            if (esc == 'n') out[o++] = '\n';
            else if (esc == 'r') out[o++] = '\r';
            else if (esc == 't') out[o++] = '\t';
            else out[o++] = esc;
            p++;
        } else {
            out[o++] = json[p++];
        }
    }
    out[o] = 0;
    return o > 0;
}

static size_t _webrtcJsonEscape(const char* in, char* out, size_t outCap) {
    size_t o = 0;
    for (const char* p = in; *p && o + 2 < outCap; p++) {
        if (*p == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (*p == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (*p == '"' || *p == '\\') { out[o++] = '\\'; out[o++] = *p; }
        else out[o++] = *p;
    }
    out[o] = 0;
    return o;
}

void RelinkIoT::_webrtcCheckSignaling() {
    if (WiFi.status() != WL_CONNECTED) return;

    String url = String(_webrtcApiBase) + "/api/device/webrtc-offer/" + _token;
    HTTPClient http;
    WiFiClient wc;
    http.begin(wc, url);
    int code = http.GET();
    if (code != 200) { http.end(); return; }
    String resp = http.getString();
    http.end();

    // static: ~5 KB on the 8 KB loop stack overflowed once DTLS keygen ran on top
    static char offerSdp[2048];
    if (!_webrtcJsonUnescapeField(resp.c_str(), "offer", offerSdp, sizeof(offerSdp))) return; // no offer waiting yet

    static char pinsCsv[1024];
    if (_webrtcJsonUnescapeField(resp.c_str(), "pins", pinsCsv, sizeof(pinsCsv))) _p2pParsePins(pinsCsv);
    char ro[4] = {0};
    _rtcReadOnly = _webrtcJsonUnescapeField(resp.c_str(), "readonly", ro, sizeof(ro)) && ro[0] == '1';

    // Each offer is a new dashboard: answer it from a fresh session -- always,
    // including the one pre-opened at boot. Reusing that one failed after a
    // reboot (the old dashboard's leftover checks had hit it first).
    if (_rtcActive) {
        // Tell the dashboard being replaced, so it stays on the relay instead
        // of answering with a new offer of its own (two tabs would otherwise
        // keep taking the one P2P session from each other).
        static const uint8_t replaced[2] = {0x22, 0};
        if (webrtcConnected()) webrtcSend(replaced, 2);
        _rtcCloseSession("new dashboard offer");
    }
    if (!_rtcActive && !_rtcOpenSession()) return;

    char mid[32] = "0";
    const char* p = strstr(offerSdp, "a=mid:");
    if (p) {
        p += 6;
        size_t i = 0;
        while (*p && *p != '\r' && *p != '\n' && i + 1 < sizeof(mid)) mid[i++] = *p++;
        mid[i] = 0;
    }

    _rtcParseOfferCandidates(offerSdp);
    String ip = WiFi.localIP().toString();
    // Our public address(es). Some networks have several public IPs (dual WAN,
    // address pools) and pick one per destination, so also ask public STUN
    // servers and offer every distinct address: the browser tries them all.
    char srflx[3 * 112] = "";
    static const struct { const char* host; uint16_t port; } stuns[] = {
        {nullptr, RELINK_STUN_PORT}, {"stun.l.google.com", 19302}, {"stun.cloudflare.com", 3478}};
    uint32_t seenIp[3]; int nSeen = 0;
    for (const auto& st : stuns) {
        uint32_t pubIp; uint16_t pubPort;
        if (!_rtcStunDiscover2(_rtcSockFd, st.host ? st.host : _server, st.port, &pubIp, &pubPort)) continue;
        bool dup = false;
        for (int i = 0; i < nSeen; i++) dup |= seenIp[i] == pubIp;
        IPAddress pub(htonl(pubIp));
        if (dup || pub == WiFi.localIP()) continue;
        seenIp[nSeen] = pubIp;
        size_t len = strlen(srflx);
        snprintf(srflx + len, sizeof(srflx) - len, "a=candidate:%d 1 UDP %u %s %u typ srflx raddr %s rport %u\r\n",
                 2 + nSeen, (unsigned)(1694498815u - nSeen), pub.toString().c_str(), (unsigned)pubPort, ip.c_str(), (unsigned)_rtcUdpPort);
        _debug("[webrtc] public address %s:%u", pub.toString().c_str(), (unsigned)pubPort);
        nSeen++;
    }
    _debug("[webrtc] %d public address(es), %d browser candidate(s) to punch", nSeen, _rtcPunchN);
    static char answerSdp[1408];
    snprintf(answerSdp, sizeof(answerSdp),
        "v=0\r\no=- 0 0 IN IP4 %s\r\ns=-\r\nt=0 0\r\n"
        "a=group:BUNDLE %s\r\n"
        "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
        "c=IN IP4 %s\r\na=ice-lite\r\na=mid:%s\r\na=sendrecv\r\n"
        "a=sctp-port:5000\r\na=max-message-size:1200\r\n"
        "a=candidate:1 1 UDP 2130706431 %s %u typ host\r\n"
        "%s"
        "a=end-of-candidates\r\na=ice-ufrag:relink\r\na=ice-pwd:%s\r\n"
        "a=fingerprint:sha-256 %s\r\na=setup:passive\r\n",
        ip.c_str(), mid, ip.c_str(), mid, ip.c_str(), (unsigned)_rtcUdpPort, srflx, _rtcIcePwd, webrtcFingerprint());

    static char escaped[2048];
    _webrtcJsonEscape(answerSdp, escaped, sizeof(escaped));

    String postUrl = String(_webrtcApiBase) + "/api/device/webrtc-answer/" + _token;
    HTTPClient http2;
    WiFiClient wc2;
    http2.begin(wc2, postUrl);
    http2.addHeader("Content-Type", "application/json");
    String body = String("{\"sdp\":\"") + escaped + "\"}";
    int code2 = http2.POST(body);
    http2.end();

    if (code2 == 200) {
        _rtcAnswered = true;
        _rtcAnswerTime = millis();
        _debug("[webrtc] posted SDP answer (mid=%s)", mid);
    } else {
        _debug("[webrtc] posting SDP answer failed, http=%d", code2);
    }
}

void RelinkIoT::_webrtcRefreshPins() {
    if (WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    WiFiClient wc;
    http.setTimeout(1500);   // runs from loop(): keep a slow API from stalling the device
    http.begin(wc, String(_webrtcApiBase) + "/api/device/allowed-pins/" + _token);
    int code = http.GET();
    if (code != 200) { http.end(); return; }
    String resp = http.getString();
    http.end();
    char pinsCsv[1024];
    if (_webrtcJsonUnescapeField(resp.c_str(), "pins", pinsCsv, sizeof(pinsCsv))) _p2pParsePins(pinsCsv);
}

bool RelinkIoT::webrtcSend(const uint8_t* data, size_t len) {
    if (!_rtcActive) return false;
    RTC_LOCK();
    bool ok = rtc_session_send(&_rtcSession, data, len) == 0;
    RTC_UNLOCK();
    return ok;
}

bool RelinkIoT::webrtcConnected() {
    return _rtcActive && _rtcSession.state == RTC_SESSION_READY;
}

// Connected AND the browser was heard from recently (it sends a keep-alive
// every second). A link that went quiet (WiFi, tab asleep) stops carrying
// data within RELINK_P2P_HEALTHY_MS -- pins go back to the relay -- but the
// session stays open, so traffic returns to P2P the moment the browser is
// heard again, with no new handshake.
bool RelinkIoT::_p2pHealthy() {
    return webrtcConnected() && (uint32_t)millis() - _rtcSession.last_rx_ms < RELINK_P2P_HEALTHY_MS;
}

bool RelinkIoT::webrtcConnecting() {
    return _rtcActive && _rtcAnswered && _rtcSession.state != RTC_SESSION_READY;
}

const char* RelinkIoT::webrtcFingerprint() {
    return _rtcSession.dtls.local_fingerprint;
}

void RelinkIoT::webrtcOnOpen(RelinkCallback cb) { _webrtcOnOpenCb = cb; }
void RelinkIoT::webrtcOnMessage(std::function<void(const uint8_t*, size_t)> cb) { _webrtcOnMessageCb = cb; }
#endif

#ifdef RELINK_CAMERA_STREAM
// ── Camera stream ────────────────────────────────────────────────────────────
// Is the current owner still watching? (A lapsed one releases the stream; it
// isn't handed to anyone else -- the next viewer to start takes it.)
bool RelinkIoT::_imageWatched() {
#ifdef RELINK_HAS_WEBRTC
    if (_camOwner == CAM_BROWSER) {
        if (_camBrowser && webrtcConnected()) return true;
        _camOwner = CAM_NONE;
    }
#endif
#ifdef RELINK_HAS_ENCRYPTION
    if (_camOwner >= 0 && _camOwner < RELINK_MESH_PEERS) {
        MeshPeer& m = _mesh[_camOwner];
        if (_encEnabled && m.used && m.inst == _camOwnerInst && m.cam()) return true;
        _camOwner = CAM_NONE;
    }
#endif
    return false;
}

// New viewer: the previous owner hears it was replaced (browser 0x21, mesh
// peer 'K') and gets nothing more until it starts watching again.
void RelinkIoT::_camTake(int8_t owner) {
    int8_t old = _camOwner;
    if (old == owner && (owner == CAM_BROWSER || _mesh[owner].inst == _camOwnerInst)) return;
#ifdef RELINK_HAS_WEBRTC
    if (old == CAM_BROWSER && webrtcConnected()) {
        static const uint8_t kick[2] = {0x21, 0};
        webrtcSend(kick, 2);
        _camBrowser = false;   // it must send 0x12 1 again to take the stream back
    }
#endif
#ifdef RELINK_HAS_ENCRYPTION
    if (old >= 0 && old < RELINK_MESH_PEERS && _mesh[old].used && _mesh[old].inst == _camOwnerInst) {
        _sendEncryptedTo('K', nullptr, 0, _mesh[old].addr, _mesh[old].addrPort);
        _mesh[old].camRx = 0;
    }
    if (owner >= 0 && owner < RELINK_MESH_PEERS) _camOwnerInst = _mesh[owner].inst;
#endif
    _camOwner = owner;
    _debug("[cam] viewer: %s", owner == CAM_BROWSER ? "dashboard" : "mesh peer");
}

bool RelinkIoT::virtualImageWanted() {
    return !_camBusy && _imageWatched()
        && (!_camMaxFps || millis() - _camLastSend >= 1000UL / _camMaxFps);
}

bool RelinkIoT::virtualImageWrite(uint8_t pin, const uint8_t* data, size_t len) {
    if (!virtualImageWrite(pin, data, len, nullptr, nullptr)) return false;
    while (_camBusy) vTaskDelay(1);
    return true;
}

static void _camReturnFb(void* fb) { esp_camera_fb_return((camera_fb_t*)fb); }

bool RelinkIoT::virtualImageWrite(uint8_t pin, camera_fb_t* fb) {
    return fb && virtualImageWrite(pin, fb->buf, fb->len, _camReturnFb, fb);
}

// The fd behind _udp (WiFiUDP keeps it private): the sender task sends from
// the very socket peers already know, so their replies still reach run().
// lwIP sockets may be used from several tasks at once.
int RelinkIoT::_camUdpFd() {
    static int fd = -1;
    sockaddr_in a{}; socklen_t al = sizeof(a);
    if (fd >= 0 && getsockname(fd, (sockaddr*)&a, &al) == 0 && a.sin_port == htons(_udpPort)) return fd;
    fd = -1;
    for (int i = LWIP_SOCKET_OFFSET; i < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; i++) {
        int type = 0; socklen_t tl = sizeof(type);
        al = sizeof(a);
        if (getsockopt(i, SOL_SOCKET, SO_TYPE, &type, &tl) != 0 || type != SOCK_DGRAM) continue;
        if (getsockname(i, (sockaddr*)&a, &al) == 0 && a.sin_port == htons(_udpPort)) { fd = i; break; }
    }
    return fd;
}

// Fills in the job and wakes the sender task on the other core; everything
// that touches shared state (owner, nonce counter, socket lookup) happens
// here, on the caller's core, before the hand-off.
bool RelinkIoT::virtualImageWrite(uint8_t pin, const uint8_t* jpeg, size_t len, void (*release)(void*), void* ctx) {
    if (!jpeg || !len || !virtualImageWanted()) return false;
    CamJob& j = _camJob;
    j.jpeg = jpeg; j.len = len; j.pin = pin;
    j.cnt = (uint16_t)((len + RELINK_CAM_CHUNK - 1) / RELINK_CAM_CHUNK);
    j.release = release; j.ctx = ctx;
    j.mesh = false;
#ifdef RELINK_HAS_ENCRYPTION
    if (_camOwner >= 0 && _camOwner < RELINK_MESH_PEERS) {
        j.fd = _camUdpFd();
        if (j.fd < 0) return false;
        MeshPeer& m = _mesh[_camOwner];
        j.mesh = true;
        j.ip = (uint32_t)m.addr;
        j.port = htons(m.addrPort);
        // cnt nonces reserved here, so the sender never touches the counter.
        uint64_t first = _nonceCounter + 1;
        _nonceCounter += j.cnt;
        if ((first >> 8) != (_nonceCounter >> 8)) _persistNonce();
        j.nonce = ((uint64_t)_instance << 48) | (first & 0xFFFFFFFFFFFFULL);
    }
#endif
    j.fid = _camFrame++;
    _camLastSend = millis();
    // gap = sender idle until this frame arrived (capture + loop time)
    if (_camStats.idleAt) _camStats.gapUs += (uint32_t)(esp_timer_get_time() - _camStats.idleAt);
    _camStatsPrint();
    _camBusy = true;
    if (!_camTask) {
        TaskHandle_t t = nullptr;
#if CONFIG_FREERTOS_UNICORE
        BaseType_t core = 0;
#else
        BaseType_t core = xPortGetCoreID() ? 0 : 1;   // the core this loop isn't on
#endif
        xTaskCreatePinnedToCore(_camTaskFn, "relink_img", 4096, this, 2, &t, core);
        _camTask = t;
    }
    xTaskNotifyGive((TaskHandle_t)_camTask);
    return true;
}

void RelinkIoT::_camTaskFn(void* arg) {
    RelinkIoT* self = (RelinkIoT*)arg;
    static uint8_t pkt[13 + 8 + RELINK_CAM_CHUNK + 16];
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        CamJob& j = self->_camJob;
        int64_t t0 = esp_timer_get_time();
        if (j.mesh) self->_camSendMesh(pkt);
        else self->_camSendBrowser();
        if (j.release) j.release(j.ctx);
        int64_t t1 = esp_timer_get_time();
        CamStats& st = self->_camStats;
        st.frames++; st.bytes += j.len; st.sendUs += (uint32_t)(t1 - t0);
        st.idleAt = t1;
        self->_camBusy = false;
    }
}

// Where each frame's time goes, per 2 s: send = sender task per frame (lock =
// waiting for run() on the session), gap = sender idle waiting for the next
// capture. send ~ 1000/fps -> network-bound; gap large -> capture-bound.
void RelinkIoT::_camStatsPrint() {
    CamStats& st = _camStats;
    int64_t now = esp_timer_get_time();
    if (!st.printAt) st.printAt = now;
    if (now - st.printAt < 2000000) return;
    float sec = (now - st.printAt) / 1e6f;
    uint32_t f = st.frames ? st.frames : 1;
#ifdef RELINK_HAS_WEBRTC
    uint32_t retries = st.retries + rtc_io_retries; rtc_io_retries = 0;
#else
    uint32_t retries = st.retries;
#endif
    _debug("[cam] %.1f fps  %u KB/frame  %u slices/s  send %.1f ms (lock %.1f)  gap %.1f ms  retries %u",
        st.frames / sec, (unsigned)(st.bytes / f / 1024), (unsigned)((st.bytes / RELINK_CAM_CHUNK + st.frames) / sec),
        st.sendUs / f / 1000.f, st.lockUs / f / 1000.f, st.gapUs / f / 1000.f, (unsigned)retries);
    int64_t idle = st.idleAt;
    st = CamStats{}; st.printAt = now; st.idleAt = idle;
}

// To the dashboard over its data channel, slice by slice under the session
// lock (run() keeps polling the same session meanwhile).
void RelinkIoT::_camSendBrowser() {
#ifdef RELINK_HAS_WEBRTC
    const CamJob& j = _camJob;
    uint8_t hdr[8] = {0x20, j.pin, (uint8_t)j.fid, (uint8_t)(j.fid >> 8), 0, 0, (uint8_t)j.cnt, (uint8_t)(j.cnt >> 8)};
    for (uint16_t i = 0; i < j.cnt; i++) {
        size_t off = (size_t)i * RELINK_CAM_CHUNK;
        size_t sz = j.len - off < RELINK_CAM_CHUNK ? j.len - off : RELINK_CAM_CHUNK;
        hdr[4] = i; hdr[5] = i >> 8;
        int64_t l0 = esp_timer_get_time();
        RTC_LOCK();
        _camStats.lockUs += (uint32_t)(esp_timer_get_time() - l0);
        bool up = _rtcActive && _rtcSession.state == RTC_SESSION_READY;
        if (up) rtc_session_send_cam(&_rtcSession, hdr, 8, j.jpeg + off, sz);
        RTC_UNLOCK();
        if (!up) return;   // session closed mid-frame
    }
#endif
}

// To the mesh owner, encrypted straight from the frame buffer into `pkt`:
// ['E'][device_id][nonce]['J' hdr + slice][tag].
void RelinkIoT::_camSendMesh(uint8_t* pkt) {
#ifdef RELINK_HAS_ENCRYPTION
    const CamJob& j = _camJob;
    uint8_t hdr[8] = {'J', j.pin, (uint8_t)j.fid, (uint8_t)(j.fid >> 8), 0, 0, (uint8_t)j.cnt, (uint8_t)(j.cnt >> 8)};
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = j.ip;
    to.sin_port = j.port;
    ChaChaPoly ccp;
    for (uint16_t i = 0; i < j.cnt; i++) {
        size_t off = (size_t)i * RELINK_CAM_CHUNK;
        size_t sz = j.len - off < RELINK_CAM_CHUNK ? j.len - off : RELINK_CAM_CHUNK;
        hdr[4] = i; hdr[5] = i >> 8;
        uint64_t nonce = j.nonce + i;
        pkt[0] = 'E';
        memcpy(pkt + 1, &_deviceId, 4);
        memcpy(pkt + 5, &nonce, 8);
        ccp.clear();
        ccp.setKey(_encKey, 32);
        ccp.setIV(pkt + 1, 12);
        ccp.encrypt(pkt + 13, hdr, 8);
        ccp.encrypt(pkt + 21, j.jpeg + off, sz);
        ccp.computeTag(pkt + 21 + sz, 16);
        // Full WiFi TX queue: give it a moment rather than drop the slice
        // (a lost slice loses the whole frame).
        for (int t = 0; t < 4; t++) {
            if (sendto(j.fd, pkt, 21 + sz + 16, 0, (sockaddr*)&to, sizeof(to)) >= 0) break;
            _camStats.retries++;
            vTaskDelay(1);
        }
    }
#endif
}

// ── cameraApply: widget settings -> sensor ──────────────────────────────────
void RelinkIoT::cameraApply(const RelinkParam& p) {
    if (p.isBytes()) _cameraApplyRecord(p.asBytes(), p.length());
    else if (p.isString()) _cameraApplyText(p.c_str(), p.length());
}

static const framesize_t CAM_SIZES[] = {   // webapp CAMERA_RESOLUTIONS order
    FRAMESIZE_QVGA, FRAMESIZE_CIF, FRAMESIZE_VGA, FRAMESIZE_SVGA,
    FRAMESIZE_XGA, FRAMESIZE_HD, FRAMESIZE_SXGA, FRAMESIZE_UXGA,
};

// 10-byte record v1 (webapp lib/cameraSettings.ts):
// [1][res][quality][bright+2][contrast+2][sat+2][flags][maxFps][aeLevel+2][effect]
// flags: bit0 mirror, bit1 flip, bit2 auto white balance, bit3 auto exposure
void RelinkIoT::_cameraApplyRecord(const uint8_t* r, size_t len) {
    if (len < 10 || r[0] != 1) { _debug("[cam] settings record ignored (len %u)", (unsigned)len); return; }
    _camMaxFps = r[7];
    sensor_t* s = esp_camera_sensor_get();
    if (!s) return;
    if (r[1] < sizeof(CAM_SIZES) / sizeof(CAM_SIZES[0])) s->set_framesize(s, CAM_SIZES[r[1]]);
    s->set_quality(s, constrain((int)r[2], 4, 63));
    s->set_brightness(s, constrain((int)r[3] - 2, -2, 2));
    s->set_contrast(s, constrain((int)r[4] - 2, -2, 2));
    s->set_saturation(s, constrain((int)r[5] - 2, -2, 2));
    s->set_hmirror(s, r[6] & 1);
    s->set_vflip(s, (r[6] >> 1) & 1);
    s->set_whitebal(s, (r[6] >> 2) & 1);
    s->set_exposure_ctrl(s, (r[6] >> 3) & 1);
    s->set_ae_level(s, constrain((int)r[8] - 2, -2, 2));
    s->set_special_effect(s, constrain((int)r[9], 0, 6));
    _debug("[cam] settings: size %u q %u b %d c %d s %d flags %x fps %u ae %d fx %u",
           r[1], r[2], r[3] - 2, r[4] - 2, r[5] - 2, r[6], r[7], r[8] - 2, r[9]);
}

// "fs=VGA;q=12;b=0;c=0;s=0;hm=0;vf=0;fps=0;awb=1;aec=1;ael=0;fx=0" -- any
// subset, any order. Unknown keys are ignored.
void RelinkIoT::_cameraApplyText(const char* str, size_t len) {
    sensor_t* s = esp_camera_sensor_get();
    char buf[256];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, str, len); buf[len] = 0;
    static const char* const names[] = {"QVGA", "CIF", "VGA", "SVGA", "XGA", "HD", "SXGA", "UXGA"};
    for (char* save = nullptr, *kv = strtok_r(buf, ";", &save); kv; kv = strtok_r(nullptr, ";", &save)) {
        char* eq = strchr(kv, '=');
        if (!eq) continue;
        *eq = 0;
        const char* k = kv; const char* v = eq + 1;
        int n = atoi(v);
        if (!strcmp(k, "fps")) { _camMaxFps = n > 0 ? n : 0; continue; }
        if (!s) continue;
        if (!strcmp(k, "fs")) {
            for (size_t z = 0; z < 8; z++) if (!strcmp(v, names[z])) { s->set_framesize(s, CAM_SIZES[z]); break; }
        }
        else if (!strcmp(k, "q"))   s->set_quality(s, constrain(n, 4, 63));
        else if (!strcmp(k, "b"))   s->set_brightness(s, constrain(n, -2, 2));
        else if (!strcmp(k, "c"))   s->set_contrast(s, constrain(n, -2, 2));
        else if (!strcmp(k, "s"))   s->set_saturation(s, constrain(n, -2, 2));
        else if (!strcmp(k, "hm"))  s->set_hmirror(s, n ? 1 : 0);
        else if (!strcmp(k, "vf"))  s->set_vflip(s, n ? 1 : 0);
        else if (!strcmp(k, "awb")) s->set_whitebal(s, n ? 1 : 0);
        else if (!strcmp(k, "aec")) s->set_exposure_ctrl(s, n ? 1 : 0);
        else if (!strcmp(k, "ael")) s->set_ae_level(s, constrain(n, -2, 2));
        else if (!strcmp(k, "fx"))  s->set_special_effect(s, constrain(n, 0, 6));
    }
    _debug("[cam] settings applied: %.*s", (int)len, str);
}

#endif // RELINK_CAMERA_STREAM

// ── Debug ─────────────────────────────────────────────────────────────────────
void RelinkIoT::_debug(const char* fmt, ...) {
    if (!_debugEnabled) return;
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.println(buf);
}
