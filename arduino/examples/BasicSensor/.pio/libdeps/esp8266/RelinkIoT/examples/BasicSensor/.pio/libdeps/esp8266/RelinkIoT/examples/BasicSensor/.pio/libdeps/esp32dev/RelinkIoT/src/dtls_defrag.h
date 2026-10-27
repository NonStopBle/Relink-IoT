#ifndef DTLS_DEFRAG_H
#define DTLS_DEFRAG_H
/*
 * dtls_defrag -- reassembles a fragmented DTLS ClientHello (RFC 6347
 * S4.2.3) before handing it to mbedTLS.
 *
 * Real browsers' ClientHello (full real cipher-suite list, GREASE,
 * extensions) commonly exceeds ~1200 bytes and gets fragmented across
 * two+ UDP datagrams. mbedTLS 2.28.x's server-side parser explicitly
 * refuses to reassemble a fragmented ClientHello specifically (its own
 * source comment: "We don't support fragmentation of ClientHello
 * (yet?)") -- every OTHER handshake message type it reassembles fine.
 * This sits in front of it and does just that one missing piece.
 *
 * Single in-flight reassembly, keyed by DTLS message_seq -- sufficient
 * for our one-peer-at-a-time use case (a new message_seq resets state,
 * so a retried ClientHello after HelloVerifyRequest starts fresh
 * correctly).
 */
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DTLS_DEFRAG_MAX_MSG 2048 /* generous ceiling for a ClientHello */

typedef struct {
    int active;
    uint16_t message_seq;
    uint32_t total_len;
    uint8_t buffer[DTLS_DEFRAG_MAX_MSG];
    uint8_t covered[DTLS_DEFRAG_MAX_MSG];
    uint8_t record_prefix[13]; /* most recent fragment's DTLS record header, reused verbatim */

    uint8_t ready_buf[13 + 12 + DTLS_DEFRAG_MAX_MSG];
} dtls_defrag_t;

void dtls_defrag_init(dtls_defrag_t* d);

/* Feed one raw UDP datagram. Returns:
 *   0 -- not a ClientHello fragment (not handshake content, not type 1,
 *        or already a complete single-record ClientHello) -- caller
 *        should pass `in` straight through to mbedTLS unchanged.
 *   1 -- a fragment was consumed and buffered, message still incomplete
 *        -- caller should treat this UDP read as "nothing to deliver".
 *   2 -- this fragment completed the message -- out/out_len give a
 *        synthesized, unfragmented record ready for mbedTLS. *out points
 *        into `d`, valid until the next dtls_defrag_feed() call. */
int dtls_defrag_feed(dtls_defrag_t* d, const uint8_t* in, size_t in_len,
                      const uint8_t** out, size_t* out_len);

#ifdef __cplusplus
}
#endif
#endif
