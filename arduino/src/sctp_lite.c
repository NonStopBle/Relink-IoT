#include "webrtc_lite_config.h"
#ifdef RELINK_HAS_WEBRTC
#include "sctp_lite.h"

#include <string.h>

/* ---- wire format constants (RFC 4960) ---- */
#define SCTP_CHUNK_DATA        0
#define SCTP_CHUNK_INIT        1
#define SCTP_CHUNK_INIT_ACK    2
#define SCTP_CHUNK_SACK        3
#define SCTP_CHUNK_ABORT       6
#define SCTP_CHUNK_COOKIE_ECHO 10
#define SCTP_CHUNK_COOKIE_ACK  11

#define SCTP_DATA_LAST_FRAG  0x01
#define SCTP_DATA_FIRST_FRAG 0x02
#define SCTP_DATA_UNORDERED  0x04

#define SCTP_PARAM_STATE_COOKIE 7

#define COMMON_HDR_LEN 12
#define CHUNK_HDR_LEN  4

/* We are never the SCTP association initiator (see header rationale), so the
 * cookie only needs to survive our own INIT-ACK -> COOKIE-ECHO round trip --
 * it is not required to be unguessable. */
typedef struct {
    uint32_t local_tag;
    uint32_t peer_tag;
    uint32_t peer_initial_tsn;
    uint16_t peer_outbound_streams;
    uint16_t peer_inbound_streams;
} sctp_lite_cookie_t;

/* ---- CRC32c (Castagnoli), reflected table -- RFC 3309 / RFC 4960 App B ----
 * SCTP's checksum field is the one field in the packet that's written in
 * host (little-endian on every platform we target) byte order rather than
 * network order -- this isn't a typo, every interoperable SCTP stack
 * (usrsctp included) does this; it's a long-standing wire-format quirk. */
static uint32_t crc32c_table[256];
static int crc32c_table_ready = 0;

static void crc32c_build_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        crc32c_table[i] = c;
    }
    crc32c_table_ready = 1;
}

static uint32_t crc32c(const uint8_t* data, size_t len) {
    if (!crc32c_table_ready) crc32c_build_table();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = crc32c_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ---- byte helpers (big-endian wire format, except the checksum) ---- */
static void put_u16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put_u32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint16_t get_u16(const uint8_t* p) { return ((uint16_t)p[0] << 8) | p[1]; }
static uint32_t get_u32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static size_t pad4(size_t len) { return (len + 3) & ~((size_t)3); }

static void finalize_and_send(sctp_lite_assoc_t* a, uint8_t* packet, size_t len) {
    put_u32(packet + 8, 0); /* zero checksum field before computing */
    uint32_t sum = crc32c(packet, len);
    /* little-endian store, see crc32c() comment above */
    packet[8]  = (uint8_t)(sum);
    packet[9]  = (uint8_t)(sum >> 8);
    packet[10] = (uint8_t)(sum >> 16);
    packet[11] = (uint8_t)(sum >> 24);
    a->send_cb(a->user, packet, len);
}

static void write_common_header(uint8_t* p, uint32_t verification_tag) {
    put_u16(p, 5000);      /* source port -- unused at this layer, fixed per WebRTC convention */
    put_u16(p + 2, 5000);  /* dest port */
    put_u32(p + 4, verification_tag);
    put_u32(p + 8, 0);     /* checksum placeholder */
}

void sctp_lite_init(sctp_lite_assoc_t* a, uint32_t local_verification_tag,
                     uint32_t local_initial_tsn, sctp_lite_send_cb send_cb,
                     sctp_lite_deliver_cb deliver_cb, void* user) {
    memset(a, 0, sizeof(*a));
    a->state = SCTP_LITE_CLOSED;
    a->local_verification_tag = local_verification_tag;
    a->local_initial_tsn = local_initial_tsn;
    a->next_tsn_to_send = local_initial_tsn;
    a->send_cb = send_cb;
    a->deliver_cb = deliver_cb;
    a->user = user;
}

static void send_init_ack(sctp_lite_assoc_t* a, uint32_t peer_tag,
                           uint32_t peer_initial_tsn, uint16_t peer_out,
                           uint16_t peer_in) {
    uint8_t pkt[COMMON_HDR_LEN + CHUNK_HDR_LEN + 16 + 4 + sizeof(sctp_lite_cookie_t)];
    write_common_header(pkt, peer_tag); /* verification tag = peer's tag until established */

    uint8_t* chunk = pkt + COMMON_HDR_LEN;
    sctp_lite_cookie_t cookie;
    cookie.local_tag = a->local_verification_tag;
    cookie.peer_tag = peer_tag;
    cookie.peer_initial_tsn = peer_initial_tsn;
    cookie.peer_outbound_streams = peer_out;
    cookie.peer_inbound_streams = peer_in;

    size_t fixed_len = 16; /* initiate tag, a_rwnd, #out, #in, initial tsn */
    size_t cookie_param_len = 4 + sizeof(cookie); /* param hdr + value */
    size_t chunk_value_len = fixed_len + cookie_param_len;
    size_t chunk_len = CHUNK_HDR_LEN + chunk_value_len;

    chunk[0] = SCTP_CHUNK_INIT_ACK;
    chunk[1] = 0;
    put_u16(chunk + 2, (uint16_t)chunk_len);

    uint8_t* v = chunk + CHUNK_HDR_LEN;
    put_u32(v, a->local_verification_tag);  /* our initiate tag */
    put_u32(v + 4, 131072);                 /* a_rwnd */
    /* Streams: one per data channel. The browser numbers its channels 0, 2,
     * 4... (it is the DTLS client), so a few streams cover the pin channel
     * plus a separate camera channel. */
    put_u16(v + 8, SCTP_LITE_STREAMS);      /* outbound streams */
    put_u16(v + 10, SCTP_LITE_STREAMS);     /* inbound streams */
    put_u32(v + 12, a->local_initial_tsn);

    uint8_t* param = v + 16;
    put_u16(param, SCTP_PARAM_STATE_COOKIE);
    put_u16(param + 2, (uint16_t)cookie_param_len);
    memcpy(param + 4, &cookie, sizeof(cookie));

    size_t total = COMMON_HDR_LEN + chunk_len; /* no padding needed, all fields already 4-byte aligned */
    finalize_and_send(a, pkt, total);
}

static void send_cookie_ack(sctp_lite_assoc_t* a) {
    uint8_t pkt[COMMON_HDR_LEN + CHUNK_HDR_LEN];
    write_common_header(pkt, a->peer_verification_tag);
    pkt[COMMON_HDR_LEN + 0] = SCTP_CHUNK_COOKIE_ACK;
    pkt[COMMON_HDR_LEN + 1] = 0;
    put_u16(pkt + COMMON_HDR_LEN + 2, CHUNK_HDR_LEN);
    finalize_and_send(a, pkt, sizeof(pkt));
}

static void send_sack(sctp_lite_assoc_t* a) {
    uint8_t pkt[COMMON_HDR_LEN + CHUNK_HDR_LEN + 12];
    write_common_header(pkt, a->peer_verification_tag);
    uint8_t* chunk = pkt + COMMON_HDR_LEN;
    chunk[0] = SCTP_CHUNK_SACK;
    chunk[1] = 0;
    put_u16(chunk + 2, CHUNK_HDR_LEN + 12);
    uint8_t* v = chunk + CHUNK_HDR_LEN;
    put_u32(v, a->cumulative_tsn_ack);
    put_u32(v + 4, 131072);  /* a_rwnd */
    put_u16(v + 8, 0);       /* num gap ack blocks -- unordered/unreliable, we don't track gaps */
    put_u16(v + 10, 0);      /* num dup tsns */
    finalize_and_send(a, pkt, sizeof(pkt));
}

static void handle_init(sctp_lite_assoc_t* a, const uint8_t* v, size_t vlen) {
    if (vlen < 16) return;
    uint32_t peer_tag = get_u32(v);
    uint32_t peer_initial_tsn = get_u32(v + 12);
    uint16_t peer_out = get_u16(v + 8);
    uint16_t peer_in = get_u16(v + 10);
    send_init_ack(a, peer_tag, peer_initial_tsn, peer_out, peer_in);
    a->state = SCTP_LITE_WAIT_COOKIE_ECHO;
}

static void handle_cookie_echo(sctp_lite_assoc_t* a, const uint8_t* v, size_t vlen) {
    if (vlen < sizeof(sctp_lite_cookie_t)) return;
    sctp_lite_cookie_t cookie;
    memcpy(&cookie, v, sizeof(cookie));
    if (cookie.local_tag != a->local_verification_tag) return; /* not our cookie */

    a->peer_verification_tag = cookie.peer_tag;
    a->peer_initial_tsn = cookie.peer_initial_tsn;
    a->cumulative_tsn_ack = cookie.peer_initial_tsn - 1; /* nothing received yet */
    a->peer_outbound_streams = cookie.peer_outbound_streams;
    a->peer_inbound_streams = cookie.peer_inbound_streams;
    a->state = SCTP_LITE_ESTABLISHED;
    send_cookie_ack(a);
}

static void handle_data(sctp_lite_assoc_t* a, const uint8_t* v, size_t vlen) {
    if (vlen < 12) return;
    uint32_t tsn = get_u32(v);
    uint16_t stream_id = get_u16(v + 4);
    uint32_t ppid = get_u32(v + 8);
    const uint8_t* payload = v + 12;
    size_t payload_len = vlen - 12;

    /* unordered/unreliable: accept out of order, just advance the
     * contiguous-ack point opportunistically (good enough for a SACK, no
     * gap-block bookkeeping since nothing gets retransmitted anyway). */
    int32_t delta = (int32_t)(tsn - a->cumulative_tsn_ack);
    if (delta == 1) {
        a->cumulative_tsn_ack = tsn;
    } else if (delta > 1) {
        a->cumulative_tsn_ack = tsn; /* accept the gap; PR-SCTP peer won't retransmit anyway */
    } /* delta <= 0: duplicate/old TSN, still deliver below, just don't move the ack point back */

    if (a->deliver_cb) a->deliver_cb(a->user, stream_id, ppid, payload, payload_len);
    send_sack(a);
}

void sctp_lite_input(sctp_lite_assoc_t* a, const uint8_t* buf, size_t len) {
    if (len < COMMON_HDR_LEN) return;

    size_t pos = COMMON_HDR_LEN;
    while (pos + CHUNK_HDR_LEN <= len) {
        uint8_t type = buf[pos];
        uint16_t chunk_len = get_u16(buf + pos + 2);
        if (chunk_len < CHUNK_HDR_LEN || pos + chunk_len > len) break;
        const uint8_t* v = buf + pos + CHUNK_HDR_LEN;
        size_t vlen = chunk_len - CHUNK_HDR_LEN;

        switch (type) {
            case SCTP_CHUNK_INIT:        handle_init(a, v, vlen); break;
            case SCTP_CHUNK_COOKIE_ECHO: handle_cookie_echo(a, v, vlen); break;
            case SCTP_CHUNK_DATA:        handle_data(a, v, vlen); break;
            case SCTP_CHUNK_SACK:        /* we don't retransmit, nothing to do with peer's SACKs */ break;
            case SCTP_CHUNK_ABORT:       a->state = SCTP_LITE_CLOSED; break;
            default: break; /* unknown chunk types are ignored, not fatal */
        }
        pos += pad4(chunk_len);
    }
}

int sctp_lite_send(sctp_lite_assoc_t* a, uint16_t stream_id, uint32_t ppid,
                    const uint8_t* payload, size_t len) {
    return sctp_lite_send2(a, stream_id, ppid, payload, len, NULL, 0);
}

int sctp_lite_send2(sctp_lite_assoc_t* a, uint16_t stream_id, uint32_t ppid,
                     const uint8_t* hdr, size_t hlen, const uint8_t* body, size_t blen) {
    size_t len = hlen + blen;
    if (a->state != SCTP_LITE_ESTABLISHED) return -1;
    if (len > SCTP_LITE_MAX_CHUNK_PAYLOAD) return -2;

    size_t chunk_value_len = 12 + len;
    size_t chunk_len = CHUNK_HDR_LEN + chunk_value_len;
    size_t padded_chunk_len = pad4(chunk_len);
    size_t total = COMMON_HDR_LEN + padded_chunk_len;

    uint8_t pkt[COMMON_HDR_LEN + CHUNK_HDR_LEN + 12 + SCTP_LITE_MAX_CHUNK_PAYLOAD];
    write_common_header(pkt, a->peer_verification_tag);

    uint8_t* chunk = pkt + COMMON_HDR_LEN;
    chunk[0] = SCTP_CHUNK_DATA;
    chunk[1] = SCTP_DATA_FIRST_FRAG | SCTP_DATA_LAST_FRAG | SCTP_DATA_UNORDERED;
    put_u16(chunk + 2, (uint16_t)chunk_len);

    uint8_t* v = chunk + CHUNK_HDR_LEN;
    put_u32(v, a->next_tsn_to_send++);
    put_u16(v + 4, stream_id);
    put_u16(v + 6, 0); /* stream sequence number -- meaningless for unordered, kept 0 */
    put_u32(v + 8, ppid);
    memcpy(v + 12, hdr, hlen);
    if (blen) memcpy(v + 12 + hlen, body, blen);
    memset(v + 12 + len, 0, padded_chunk_len - chunk_len); /* zero the pad bytes */

    finalize_and_send(a, pkt, total);
    return 0;
}

#endif /* RELINK_HAS_WEBRTC */
