#ifndef DTLS_LITE_H
#define DTLS_LITE_H
/*
 * dtls_lite -- DTLS 1.2 transport for the SCTP association, nothing else.
 *
 * Cut down from libpeer's dtls_srtp.c: that file is written against
 * mbedTLS 3.x's export-keys API (mbedtls_ssl_set_export_keys_cb +
 * psa/crypto.h) purely to derive SRTP keys via RFC 5764. We have no media,
 * so we never need SRTP key export at all -- we only need the DTLS record
 * layer's own encryption (mbedtls_ssl_write/read) to carry SCTP packets.
 * That means this targets mbedTLS 2.28.7 (what Arduino-ESP32 actually
 * ships) directly, with no PSA crypto dependency.
 *
 * We are always DTLS_LITE_ROLE_SERVER: the SDP answer always sets
 * a=setup:passive for the ESP32 side (see sctp_lite.h for why -- this also
 * fixes which side sends ClientHello vs. waits for it, and which side's
 * SCTP engine is the INIT responder).
 *
 * This module does no socket I/O itself -- the caller supplies send/recv
 * callbacks that talk to the ICE-punched UDP socket. That keeps it unit
 * testable against a loopback pair of instances, same as sctp_lite.
 */
#include <stdint.h>
#include <stddef.h>

#if __has_include(<mbedtls/ctr_drbg.h>)
#include <mbedtls/ctr_drbg.h>
#else
#include <mbedtls/private/ctr_drbg.h>
#endif
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/x509_crt.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DTLS_LITE_FINGERPRINT_LEN 96 /* "XX:XX:...:XX\0" for SHA-256, 32 bytes -> 95 chars + nul */

typedef int (*dtls_lite_io_cb)(void* ctx, unsigned char* buf, size_t len);
/* Caller-supplied monotonic clock in milliseconds (Arduino: millis();
 * host tests: a clock_gettime/clock() wrapper). mbedTLS's DTLS flight
 * retransmission timer is mandatory -- mbedtls_ssl_handshake() refuses to
 * run at all without one set -- and we inject it rather than depend on
 * mbedtls's own timing.c so this stays host-testable without pulling in
 * POSIX-only timing code the ESP32 port may not provide. */
typedef uint32_t (*dtls_lite_now_ms_cb)(void);

typedef struct {
    uint32_t start_ms;
    uint32_t int_ms;  /* intermediate (retransmit) delay, 0 = cancelled */
    uint32_t fin_ms;  /* final (give up) delay */
    dtls_lite_now_ms_cb now_ms;
} dtls_lite_timer_t;

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cert;
    mbedtls_pk_context pkey;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_cookie_ctx cookie_ctx; /* mandatory for a DTLS server -- RFC 6347 HelloVerifyRequest anti-amplification */
    dtls_lite_timer_t timer;

    char local_fingerprint[DTLS_LITE_FINGERPRINT_LEN]; /* "AB:CD:...:EF", SHA-256, for our SDP answer */

    void* io_ctx;
    int initialized;

    uint8_t peer_id[32];
    size_t peer_id_len;
} dtls_lite_t;

/* Generates a fresh self-signed EC (P-256) cert + key and computes its
 * SHA-256 fingerprint into d->local_fingerprint. send_cb/recv_cb follow
 * mbedtls's own f_send/f_recv signature so they can be passed straight
 * into mbedtls_ssl_set_bio(). */
int dtls_lite_init(dtls_lite_t* d, dtls_lite_io_cb send_cb, dtls_lite_io_cb recv_cb,
                    void* io_ctx, dtls_lite_now_ms_cb now_ms);

void dtls_lite_deinit(dtls_lite_t* d);

/* Must be called once, after ICE has settled on the remote peer's address
 * and before the first dtls_lite_handshake_step() -- binds the DTLS cookie
 * to that peer (mbedtls_ssl_set_client_transport_id), e.g. pass the
 * candidate pair's remote ip:port bytes. */
int dtls_lite_set_peer_id(dtls_lite_t* d, const uint8_t* id, size_t len);

/* Drives the handshake. Call repeatedly (e.g. from your main loop) until it
 * returns 0 (done) or a negative mbedtls error that isn't WANT_READ/WRITE. */
int dtls_lite_handshake_step(dtls_lite_t* d);

int dtls_lite_write(dtls_lite_t* d, const uint8_t* buf, size_t len);
int dtls_lite_read(dtls_lite_t* d, uint8_t* buf, size_t len);
/* Nonzero if a record already received is still buffered (no socket read needed). */
int dtls_lite_pending(dtls_lite_t* d);

#ifdef __cplusplus
}
#endif
#endif
