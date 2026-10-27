#include "webrtc_lite_config.h"
#ifdef RELINK_HAS_WEBRTC
#include "dtls_defrag.h"

#include <string.h>

#define DTLS_RECORD_HDR_LEN 13
#define DTLS_HS_HDR_LEN 12
#define CONTENT_TYPE_HANDSHAKE 22
#define HS_TYPE_CLIENT_HELLO 1

static uint32_t get_u24(const uint8_t* p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
static void put_u16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put_u24(uint8_t* p, uint32_t v) { p[0] = v >> 16; p[1] = v >> 8; p[2] = v; }

void dtls_defrag_init(dtls_defrag_t* d) {
    memset(d, 0, sizeof(*d));
}

int dtls_defrag_feed(dtls_defrag_t* d, const uint8_t* in, size_t in_len,
                      const uint8_t** out, size_t* out_len) {
    if (in_len < DTLS_RECORD_HDR_LEN + DTLS_HS_HDR_LEN) return 0;
    if (in[0] != CONTENT_TYPE_HANDSHAKE) return 0;

    const uint8_t* hs = in + DTLS_RECORD_HDR_LEN;
    uint8_t hs_type = hs[0];
    if (hs_type != HS_TYPE_CLIENT_HELLO) return 0;

    uint32_t total_len = get_u24(hs + 1);
    uint16_t message_seq = ((uint16_t)hs[4] << 8) | hs[5];
    uint32_t frag_offset = get_u24(hs + 6);
    uint32_t frag_length = get_u24(hs + 9);

    if (frag_offset == 0 && frag_length == total_len) return 0; /* already a single, complete record */
    if (total_len > DTLS_DEFRAG_MAX_MSG) return 0; /* beyond anything a real ClientHello needs; don't buffer */
    if (DTLS_RECORD_HDR_LEN + DTLS_HS_HDR_LEN + frag_length > in_len) return 0; /* malformed, let mbedTLS reject it normally */
    if (frag_offset + frag_length > total_len) return 0; /* malformed */

    if (!d->active || d->message_seq != message_seq) {
        dtls_defrag_init(d);
        d->active = 1;
        d->message_seq = message_seq;
        d->total_len = total_len;
    }

    memcpy(d->buffer + frag_offset, hs + DTLS_HS_HDR_LEN, frag_length);
    memset(d->covered + frag_offset, 1, frag_length);
    memcpy(d->record_prefix, in, DTLS_RECORD_HDR_LEN);

    for (uint32_t i = 0; i < d->total_len; i++) {
        if (!d->covered[i]) return 1; /* still incomplete */
    }

    /* fully covered -- synthesize one unfragmented record */
    memcpy(d->ready_buf, d->record_prefix, DTLS_RECORD_HDR_LEN);
    uint8_t* out_hs = d->ready_buf + DTLS_RECORD_HDR_LEN;
    out_hs[0] = HS_TYPE_CLIENT_HELLO;
    put_u24(out_hs + 1, d->total_len);
    put_u16(out_hs + 4, d->message_seq);
    put_u24(out_hs + 6, 0);            /* fragment_offset = 0 */
    put_u24(out_hs + 9, d->total_len); /* fragment_length = total_len */
    memcpy(out_hs + DTLS_HS_HDR_LEN, d->buffer, d->total_len);

    size_t record_payload_len = DTLS_HS_HDR_LEN + d->total_len;
    put_u16(d->ready_buf + 11, (uint16_t)record_payload_len); /* record header's own length field */

    *out = d->ready_buf;
    *out_len = DTLS_RECORD_HDR_LEN + record_payload_len;

    d->active = 0; /* consumed; next differing message_seq starts fresh */
    return 2;
}

#endif /* RELINK_HAS_WEBRTC */
