#include "rtc_session.h"
#include "webrtc_lite_config.h"
#ifdef RELINK_HAS_WEBRTC

#include "stun_lite.h"
#include "dtls_defrag.h"
#include <stdio.h>
#include <sys/socket.h>
#include <errno.h>
#include <string.h>

/* Socket is bound but not connect()ed -- every send/recv targets/learns
 * s->peer_addr explicitly, since we don't know the peer's address until
 * its first STUN Binding Request arrives. */
static int io_send(void* ctx, unsigned char* buf, size_t len) {
    rtc_session_t* s = (rtc_session_t*)ctx;
    int n = (int)sendto(s->sockfd, buf, len, 0, (struct sockaddr*)&s->peer_addr, sizeof(s->peer_addr));
    if (n < 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return n;
}

static int io_recv(void* ctx, unsigned char* buf, size_t len) {
    rtc_session_t* s = (rtc_session_t*)ctx;
    struct sockaddr_in from; socklen_t fromlen = sizeof(from);
    int n = (int)recvfrom(s->sockfd, buf, len, 0, (struct sockaddr*)&from, &fromlen);
    if (n < 0) return MBEDTLS_ERR_SSL_WANT_READ;
    if (n == 0) return MBEDTLS_ERR_SSL_WANT_READ;
    /* Only the peer that passed ICE: a stray record from an earlier
     * session on this port would otherwise reach mbedTLS before the
     * ClientHello and fail the handshake (BAD_HS_CLIENT_HELLO). */
    if (from.sin_addr.s_addr != s->peer_addr.sin_addr.s_addr || from.sin_port != s->peer_addr.sin_port)
        return MBEDTLS_ERR_SSL_WANT_READ;

    const uint8_t* out; size_t out_len;
    int dr = dtls_defrag_feed(&s->defrag, buf, (size_t)n, &out, &out_len);
    if (dr == 0) return n;                       /* not a ClientHello fragment, pass through as-is */
    if (dr == 1) return MBEDTLS_ERR_SSL_WANT_READ; /* fragment buffered, nothing to deliver yet */
    /* dr == 2: message just completed -- hand mbedTLS the reassembled record */
    if (out_len > len) return MBEDTLS_ERR_SSL_WANT_READ; /* shouldn't happen (caller's buffer is mbedTLS's own, generously sized), but stay safe */
    memcpy(buf, out, out_len);
    return (int)out_len;
}

static void sctp_send_cb(void* user, const uint8_t* buf, size_t len) {
    /* sctp_lite_assoc_t has a single shared `user` pointer, and it's set
     * below to &s->dcep (not s) because dcep_lite_on_sctp_deliver -- used
     * as the deliver_cb on the same association -- requires a dcep_lite_t*
     * per its own contract. Recover rtc_session_t via dcep's own `user`,
     * which rtc_session_init sets to `s`. */
    dcep_lite_t* dcep = (dcep_lite_t*)user;
    rtc_session_t* s = (rtc_session_t*)dcep->user;
    int w = dtls_lite_write(&s->dtls, buf, len);
    if (w < 0 && w != MBEDTLS_ERR_SSL_WANT_WRITE) {
        /* Never silent again: a refused record is a lost message. */
        static uint32_t fails, last_log;
        fails++;
        uint32_t now = s->now_ms ? s->now_ms() : 0;
        if (now - last_log > 2000) {
            last_log = now;
            fprintf(stderr, "[dtls] write refused (%d, len %u), %u so far\n", w, (unsigned)len, (unsigned)fails);
        }
    }
}

static void dcep_onopen(void* user, uint16_t stream_id) {
    (void)stream_id;
    rtc_session_t* s = (rtc_session_t*)user;
    if (s->onopen) s->onopen(s->user);
}

static void dcep_onmessage(void* user, uint16_t stream_id, const uint8_t* data, size_t len) {
    (void)stream_id;
    rtc_session_t* s = (rtc_session_t*)user;
    if (s->onmessage) s->onmessage(s->user, data, len);
}

int rtc_session_init(rtc_session_t* s, int sockfd, const char* local_ice_pwd,
                      dtls_lite_now_ms_cb now_ms,
                      rtc_session_onopen_cb onopen, rtc_session_onmessage_cb onmessage,
                      void* user) {
    memset(s, 0, sizeof(*s));
    s->sockfd = sockfd;
    s->local_ice_pwd = local_ice_pwd;
    s->onopen = onopen;
    s->onmessage = onmessage;
    s->user = user;
    s->state = RTC_SESSION_ICE;
    s->now_ms = now_ms;
    s->last_rx_ms = now_ms ? now_ms() : 0;

    int ret = dtls_lite_init(&s->dtls, io_send, io_recv, s, now_ms);
    if (ret != 0) return ret;

    sctp_lite_init(&s->sctp, (uint32_t)(uintptr_t)s ^ 0x5ca1ab1e, 1, sctp_send_cb,
                    dcep_lite_on_sctp_deliver, &s->dcep);
    dcep_lite_init(&s->dcep, &s->sctp, dcep_onopen, dcep_onmessage, s);
    return 0;
}

/* Returns 1 if buf was a STUN packet (handled, response sent if it was a
 * valid Binding Request), 0 if the caller should treat it as DTLS. */
static int handle_if_stun(rtc_session_t* s, const uint8_t* buf, size_t n,
                           const struct sockaddr_in* from) {
    if (!stun_lite_looks_like_stun(buf, n)) return 0;

    uint8_t resp[128];
    size_t resp_len = stun_lite_handle_binding_request(buf, n, s->local_ice_pwd,
                                                         ntohl(from->sin_addr.s_addr), ntohs(from->sin_port),
                                                         resp, sizeof(resp));
    fprintf(stderr, "[stun] got request n=%zu from port=%d -> resp_len=%zu\n", n, ntohs(from->sin_port), resp_len);
    if (resp_len > 0) {
        sendto(s->sockfd, resp, resp_len, 0, (const struct sockaddr*)from, sizeof(*from));
        if (!s->peer_known) {
            s->peer_addr = *from;
            s->peer_known = 1;
            dtls_lite_set_peer_id(&s->dtls, (const uint8_t*)from, sizeof(*from));
            if (s->state == RTC_SESSION_ICE) s->state = RTC_SESSION_DTLS_HANDSHAKE;
        }
    }
    return 1;
}

void rtc_session_poll(rtc_session_t* s) {
    /* Drain and classify whatever's waiting: STUN gets answered here
     * (in any state -- browsers keep sending consent-freshness checks
     * after the connection is up), anything else is pushed back for the
     * DTLS layer to consume via its own io_recv. Since UDP sockets don't
     * support peeking-without-consuming cheaply across platforms, and
     * dtls_lite owns the only other reader of this socket, we peek with
     * MSG_PEEK first to classify without stealing a real DTLS record. */
    uint8_t peek[1500];
    struct sockaddr_in from; socklen_t fromlen = sizeof(from);
    ssize_t n = recvfrom(s->sockfd, peek, sizeof(peek), MSG_PEEK, (struct sockaddr*)&from, &fromlen);
    if (n > 0 && s->now_ms) s->last_rx_ms = s->now_ms();
    if (n > 0 && stun_lite_looks_like_stun(peek, (size_t)n)) {
        /* consume it for real now that we know it's ours to handle */
        recvfrom(s->sockfd, peek, sizeof(peek), 0, (struct sockaddr*)&from, &fromlen);
        handle_if_stun(s, peek, (size_t)n, &from);
        return;
    }

    if (s->state == RTC_SESSION_ICE) return; /* nothing but STUN is expected yet */

    if (s->state == RTC_SESSION_DTLS_HANDSHAKE) {
        int ret = dtls_lite_handshake_step(&s->dtls);
        if (ret == 0) {
            fprintf(stderr, "[dtls] handshake complete\n");
            s->state = RTC_SESSION_READY;
        } else if (ret != 1) {
            fprintf(stderr, "[dtls] handshake FAILED ret=%d\n", ret);
            s->state = RTC_SESSION_FAILED;
        }
        return;
    }
    if (s->state != RTC_SESSION_READY) return;
    /* Nothing arrived and nothing buffered: a read would only find that
     * out the slow way, every loop (it was ~1/4 of the CPU under load). */
    if (n <= 0 && !dtls_lite_pending(&s->dtls)) return;

    uint8_t buf[1500];
    for (int i = 0; i < 8; i++) {
        int rn = dtls_lite_read(&s->dtls, buf, sizeof(buf));
        if (rn <= 0) break;
        sctp_lite_input(&s->sctp, buf, (size_t)rn);
    }
}

int rtc_session_send(rtc_session_t* s, const uint8_t* data, size_t len) {
    if (s->state != RTC_SESSION_READY) return -1;
    return dcep_lite_send(&s->dcep, 0, data, len);
}

#endif /* RELINK_HAS_WEBRTC */
