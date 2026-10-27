#include "webrtc_lite_config.h"
#ifdef RELINK_HAS_WEBRTC
#include "dtls_lite.h"

#include <string.h>
#include <stdio.h>
#include <mbedtls/version.h>
#include <mbedtls/sha256.h>
#include <mbedtls/error.h>

static const char* PERS = "relink-dtls-lite";

static int gen_self_signed_cert(dtls_lite_t* d) {
    int ret;
    mbedtls_pk_init(&d->pkey);
    const mbedtls_pk_info_t* pk_info = mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY);
    if ((ret = mbedtls_pk_setup(&d->pkey, pk_info)) != 0) return ret;
    if ((ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(d->pkey),
                                    mbedtls_ctr_drbg_random, &d->ctr_drbg)) != 0)
        return ret;

    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &d->pkey);
    mbedtls_x509write_crt_set_issuer_key(&crt, &d->pkey); /* self-signed */

    if ((ret = mbedtls_x509write_crt_set_subject_name(&crt, "CN=relink-esp32")) != 0) goto done;
    if ((ret = mbedtls_x509write_crt_set_issuer_name(&crt, "CN=relink-esp32")) != 0) goto done;
    /* WebRTC doesn't check cert validity dates or chain of trust -- the SDP
     * a=fingerprint is the actual trust anchor -- so any fixed window works. */
    if ((ret = mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20340101000000")) != 0) goto done;

#if MBEDTLS_VERSION_MAJOR >= 3
    /* mbedTLS 3 (Arduino-ESP32 core 3.x) dropped the mpi form. */
    static const unsigned char serial_raw[] = {1};
    ret = mbedtls_x509write_crt_set_serial_raw(&crt, (unsigned char*)serial_raw, sizeof(serial_raw));
#else
    mbedtls_mpi serial;
    mbedtls_mpi_init(&serial);
    mbedtls_mpi_lset(&serial, 1);
    ret = mbedtls_x509write_crt_set_serial(&crt, &serial);
    mbedtls_mpi_free(&serial);
#endif
    if (ret != 0) goto done;

    unsigned char der[1024];
    int der_len = mbedtls_x509write_crt_der(&crt, der, sizeof(der),
                                             mbedtls_ctr_drbg_random, &d->ctr_drbg);
    if (der_len < 0) { ret = der_len; goto done; }
    unsigned char* der_start = der + sizeof(der) - der_len; /* mbedtls writes DER right-aligned in the buffer */

    if ((ret = mbedtls_x509_crt_parse_der(&d->cert, der_start, (size_t)der_len)) != 0) goto done;

    unsigned char digest[32];
    mbedtls_sha256(der_start, (size_t)der_len, digest, 0);
    char* p = d->local_fingerprint;
    for (int i = 0; i < 32; i++) {
        p += sprintf(p, i == 0 ? "%02X" : ":%02X", digest[i]);
    }
    ret = 0;

done:
    mbedtls_x509write_crt_free(&crt);
    return ret;
}

/* mbedtls_ssl_set_timer_t / mbedtls_ssl_get_timer_t contract (ssl.h):
 * set_delay(ctx, int_ms, fin_ms): fin_ms==0 cancels the timer.
 * get_delay(ctx): -1 cancelled, 0 none expired, 1 only int_ms expired,
 * 2 fin_ms expired (caller should give up / move to next flight). */
static void timer_set_delay(void* ctx, uint32_t int_ms, uint32_t fin_ms) {
    dtls_lite_timer_t* t = (dtls_lite_timer_t*)ctx;
    t->int_ms = int_ms;
    t->fin_ms = fin_ms;
    t->start_ms = fin_ms != 0 ? t->now_ms() : 0;
}

static int timer_get_delay(void* ctx) {
    dtls_lite_timer_t* t = (dtls_lite_timer_t*)ctx;
    if (t->fin_ms == 0) return -1;
    uint32_t elapsed = t->now_ms() - t->start_ms;
    if (elapsed >= t->fin_ms) return 2;
    if (elapsed >= t->int_ms) return 1;
    return 0;
}

int dtls_lite_init(dtls_lite_t* d, dtls_lite_io_cb send_cb, dtls_lite_io_cb recv_cb,
                    void* io_ctx, dtls_lite_now_ms_cb now_ms) {
    memset(d, 0, sizeof(*d));
    d->timer.now_ms = now_ms;
    mbedtls_ssl_init(&d->ssl);
    mbedtls_ssl_config_init(&d->conf);
    mbedtls_x509_crt_init(&d->cert);
    mbedtls_entropy_init(&d->entropy);
    mbedtls_ctr_drbg_init(&d->ctr_drbg);
    mbedtls_ssl_cookie_init(&d->cookie_ctx);
    d->io_ctx = io_ctx;

    int ret = mbedtls_ctr_drbg_seed(&d->ctr_drbg, mbedtls_entropy_func, &d->entropy,
                                     (const unsigned char*)PERS, strlen(PERS));
    if (ret != 0) return ret;

    if ((ret = mbedtls_ssl_cookie_setup(&d->cookie_ctx, mbedtls_ctr_drbg_random, &d->ctr_drbg)) != 0)
        return ret;

    if ((ret = gen_self_signed_cert(d)) != 0) return ret;

    if ((ret = mbedtls_ssl_config_defaults(&d->conf, MBEDTLS_SSL_IS_SERVER,
                                            MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                            MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
        return ret;

    mbedtls_ssl_conf_rng(&d->conf, mbedtls_ctr_drbg_random, &d->ctr_drbg);
    /* WebRTC's trust model is the SDP fingerprint, not a CA chain -- we
     * don't verify the peer's cert against any root, we just need the
     * handshake to complete and will compare the peer's cert fingerprint
     * against the one carried in the browser's SDP offer at the caller
     * level (not implemented here -- see note in sctp_lite integration). */
    mbedtls_ssl_conf_authmode(&d->conf, MBEDTLS_SSL_VERIFY_NONE);

    if ((ret = mbedtls_ssl_conf_own_cert(&d->conf, &d->cert, &d->pkey)) != 0) return ret;

    mbedtls_ssl_conf_dtls_cookies(&d->conf, mbedtls_ssl_cookie_write, mbedtls_ssl_cookie_check,
                                   &d->cookie_ctx);

    mbedtls_ssl_conf_read_timeout(&d->conf, 1000);

    if ((ret = mbedtls_ssl_setup(&d->ssl, &d->conf)) != 0) return ret;

    mbedtls_ssl_set_bio(&d->ssl, io_ctx, (mbedtls_ssl_send_t*)send_cb,
                         (mbedtls_ssl_recv_t*)recv_cb, NULL);
    mbedtls_ssl_set_timer_cb(&d->ssl, &d->timer, timer_set_delay, timer_get_delay);
    /* Fix our own MTU ceiling before the handshake starts. mbedTLS's DTLS
     * retransmission logic can independently guess a *smaller* in-flight
     * estimate (ssl->handshake->mtu) under real network timing, which
     * mbedtls_ssl_get_current_mtu() takes the min() against -- setting
     * ours upfront gives the pacing logic a known-good ceiling to work
     * within instead of guessing, which is what actually avoids it. */
    mbedtls_ssl_set_mtu(&d->ssl, 1200);

    d->initialized = 1;
    return 0;
}

void dtls_lite_deinit(dtls_lite_t* d) {
    if (!d->initialized) return;
    mbedtls_ssl_free(&d->ssl);
    mbedtls_ssl_config_free(&d->conf);
    mbedtls_x509_crt_free(&d->cert);
    mbedtls_pk_free(&d->pkey);
    mbedtls_ssl_cookie_free(&d->cookie_ctx);
    mbedtls_ctr_drbg_free(&d->ctr_drbg);
    mbedtls_entropy_free(&d->entropy);
    d->initialized = 0;
}

int dtls_lite_set_peer_id(dtls_lite_t* d, const uint8_t* id, size_t len) {
    if (len > sizeof(d->peer_id)) return -1;
    memcpy(d->peer_id, id, len);
    d->peer_id_len = len;
    return mbedtls_ssl_set_client_transport_id(&d->ssl, id, len);
}

int dtls_lite_handshake_step(dtls_lite_t* d) {
    int ret = mbedtls_ssl_handshake(&d->ssl);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        return 1; /* call again later, nothing wrong */
    if (ret == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
        /* mbedTLS's server-side state machine returns this unconditionally
         * forever once it's sent one HelloVerifyRequest (ssl_srv.c,
         * MBEDTLS_SSL_SERVER_HELLO_VERIFY_REQUEST_SENT case) -- it never
         * transitions itself back to parse the client's retry. The
         * documented contract is that the application calls
         * mbedtls_ssl_session_reset() on seeing this, which clears the
         * handshake state back to HELLO_REQUEST while keeping config/bio/
         * cookie/transport-id, so the next step actually parses the
         * cookie-bearing ClientHello that's already sitting in our queue. */
        int rr = mbedtls_ssl_session_reset(&d->ssl);
        if (rr != 0) return rr;
        /* session_reset() frees ssl->cli_id along with everything else;
         * re-apply it so the cookie check on the retried ClientHello still
         * binds to the right peer. */
        if (d->peer_id_len > 0) {
            rr = mbedtls_ssl_set_client_transport_id(&d->ssl, d->peer_id, d->peer_id_len);
            if (rr != 0) return rr;
        }
        return 1;
    }
    return ret; /* 0 == done, otherwise a real mbedtls error */
}

int dtls_lite_write(dtls_lite_t* d, const uint8_t* buf, size_t len) {
    return mbedtls_ssl_write(&d->ssl, buf, len);
}

int dtls_lite_pending(dtls_lite_t* d) {
    return mbedtls_ssl_check_pending(&d->ssl);
}

int dtls_lite_read(dtls_lite_t* d, uint8_t* buf, size_t len) {
    return mbedtls_ssl_read(&d->ssl, buf, len);
}

#endif /* RELINK_HAS_WEBRTC */
