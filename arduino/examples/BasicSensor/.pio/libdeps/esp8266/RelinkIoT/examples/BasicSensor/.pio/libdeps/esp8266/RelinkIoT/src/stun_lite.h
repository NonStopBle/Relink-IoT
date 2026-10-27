#ifndef STUN_LITE_H
#define STUN_LITE_H
/*
 * stun_lite -- STUN Binding Request RESPONDER only (RFC 5389 + the
 * ICE-specific bits from RFC 8445: MESSAGE-INTEGRITY, FINGERPRINT).
 *
 * We declare a=ice-lite in our SDP answer, which per RFC 8445 means the
 * peer (browser, always the "full" ICE agent in WebRTC) does all the
 * connectivity-check probing and nominates the pair; the lite side only
 * ever needs to answer Binding Requests, never originate them. That's the
 * one simplification that keeps this from needing a full ICE agent
 * (candidate pairing, priorities, controlling/controlled role machinery).
 */
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Classifier per RFC 7983 S7: DTLS record content-types occupy 20-63,
 * which (being <64) ALSO satisfies "top two bits zero" -- so that check
 * alone doesn't distinguish STUN from DTLS, it only rules out TURN
 * ChannelData/other ranges. The actual discriminator RFC 7983 specifies is
 * the DTLS range check first; only outside it do we treat top-bits-zero
 * as a STUN candidate, confirmed by the magic cookie. */
static inline int stun_lite_looks_like_stun(const uint8_t* buf, size_t len) {
    if (len < 20) return 0;
    if (buf[0] >= 20 && buf[0] <= 63) return 0; /* DTLS record content-type range */
    if ((buf[0] & 0xC0) != 0) return 0;
    uint32_t cookie = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8) | buf[7];
    return cookie == 0x2112A442u;
}

/* Validates a Binding Request (checks STUN magic cookie + that it carries
 * valid MESSAGE-INTEGRITY for local_ice_pwd) and, if valid, writes a
 * Binding Success Response (with its own MESSAGE-INTEGRITY + FINGERPRINT,
 * mapped_addr_host/mapped_port_host as XOR-MAPPED-ADDRESS) into out. Returns
 * the response length, 0 if this wasn't a valid/authenticated Binding
 * Request. out must be at least in_len + 64 bytes.
 *
 * mapped_addr_host/mapped_port_host: the peer's IPv4 address and port,
 * both in HOST byte order (e.g. ntohl(sin_addr.s_addr)/ntohs(sin_port) --
 * conversion is the caller's job). Deliberately not a struct sockaddr_in
 * here, since this module's public API shouldn't force every caller to
 * drag in platform socket headers (glibc's vs. lwIP's differ enough on
 * ESP32 to make struct sockaddr_in painful to pass across this boundary). */
size_t stun_lite_handle_binding_request(const uint8_t* in, size_t in_len,
                                         const char* local_ice_pwd,
                                         uint32_t mapped_addr_host, uint16_t mapped_port_host,
                                         uint8_t* out, size_t out_cap);

#ifdef __cplusplus
}
#endif
#endif
