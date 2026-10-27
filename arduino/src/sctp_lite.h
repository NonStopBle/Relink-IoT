#ifndef SCTP_LITE_H
#define SCTP_LITE_H
/*
 * sctp_lite -- a minimal single-association, single-stream SCTP responder,
 * sized only for one WebRTC data channel (RFC 4960 subset + RFC 8832 DCEP).
 *
 * Deliberately NOT general-purpose SCTP:
 *   - one association, a few streams (SCTP_LITE_STREAMS) -- one per data
 *     channel; each channel is still a single unordered, unreliable stream
 *   - unordered + unreliable delivery only (no retransmit queue, no
 *     fragmentation/reassembly, no multi-homing, no ASCONF)
 *   - we are always the SCTP *responder* (peer sends INIT first) -- matches
 *     choosing DTLS role=passive/server for the ESP32 side, since the SCTP
 *     association initiator is whichever side is the DTLS client
 *   - the INIT-ACK state cookie is NOT HMAC-signed. Normally the cookie stops
 *     an off-path attacker from completing a handshake without seeing the
 *     INIT-ACK (resource-exhaustion defense). Here the whole exchange only
 *     happens after a completed DTLS handshake, so the peer is already
 *     authenticated and the classic SCTP-cookie threat model doesn't apply.
 *
 * This module does no I/O itself: the caller feeds it decrypted DTLS
 * application-data records via sctp_lite_input(), and it calls send_cb()
 * with bytes to hand back to mbedtls_ssl_write(). This keeps it host-testable
 * without any network or TLS stack.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SCTP_LITE_CLOSED = 0,
    SCTP_LITE_WAIT_COOKIE_ECHO,   /* sent INIT-ACK, waiting for COOKIE-ECHO */
    SCTP_LITE_ESTABLISHED,
} sctp_lite_state_t;

typedef void (*sctp_lite_send_cb)(void* user, const uint8_t* buf, size_t len);
/* ppid 50 (0x32) is reserved for DCEP control messages (RFC 8832); the
 * caller's dcep_lite layer should intercept that ppid, everything else is
 * application data. */
typedef void (*sctp_lite_deliver_cb)(void* user, uint16_t stream_id,
                                      uint32_t ppid, const uint8_t* payload,
                                      size_t len);

typedef struct {
    sctp_lite_state_t state;
    uint32_t local_verification_tag;
    uint32_t peer_verification_tag;
    uint32_t local_initial_tsn;
    uint32_t peer_initial_tsn;
    uint32_t cumulative_tsn_ack;   /* highest contiguous TSN received */
    uint32_t next_tsn_to_send;
    uint16_t peer_outbound_streams;
    uint16_t peer_inbound_streams;

    sctp_lite_send_cb send_cb;
    sctp_lite_deliver_cb deliver_cb;
    void* user;
} sctp_lite_assoc_t;

void sctp_lite_init(sctp_lite_assoc_t* a, uint32_t local_verification_tag,
                     uint32_t local_initial_tsn, sctp_lite_send_cb send_cb,
                     sctp_lite_deliver_cb deliver_cb, void* user);

/* Feed one fully-decrypted DTLS application-data record (== one SCTP
 * packet) into the state machine. May invoke send_cb()/deliver_cb(). */
void sctp_lite_input(sctp_lite_assoc_t* a, const uint8_t* buf, size_t len);

/* Send one unordered/unreliable DATA chunk on stream_id with the given
 * payload protocol identifier. Returns 0 on success, <0 if not established
 * or payload too large for a single unfragmented chunk. */
int sctp_lite_send(sctp_lite_assoc_t* a, uint16_t stream_id, uint32_t ppid,
                    const uint8_t* payload, size_t len);

/* Same, with the payload in two parts (e.g. a small header and a slice of a
 * camera frame), gathered straight into the outgoing packet -- the packet
 * buffer is the only copy, and it is needed anyway for CRC32c and DTLS. */
int sctp_lite_send2(sctp_lite_assoc_t* a, uint16_t stream_id, uint32_t ppid,
                     const uint8_t* hdr, size_t hlen, const uint8_t* body, size_t blen);

#define SCTP_LITE_STREAMS 16
#define SCTP_LITE_MAX_CHUNK_PAYLOAD 1200 /* keeps us under typical PMTU, no fragmentation support */

#ifdef __cplusplus
}
#endif
#endif
