#ifndef RTC_SESSION_H
#define RTC_SESSION_H
/*
 * rtc_session -- glues stun_lite + dtls_lite + sctp_lite + dcep_lite onto
 * one UDP socket.
 *
 * ICE role: we are always ice-lite (declared via a=ice-lite in our SDP
 * answer) -- we never originate a connectivity check, only answer the
 * peer's Binding Requests (RFC 8445 S4.2.1 permits this for the ice-lite
 * case: a lite implementation never needs to probe since it only ever
 * exists on a single, publicly-reachable-enough-for-the-peer address).
 * That keeps us to a STUN *responder*, never an agent with candidate
 * pairing/priorities/nomination of our own to implement.
 *
 * The socket is bound but NOT connect()ed: the peer's address is learned
 * from the first authenticated Binding Request's source address, after
 * which DTLS and everything above it talks to that learned address via
 * sendto/recvfrom.
 */
#include <stdint.h>
#include <stddef.h>
#if defined(ESP_PLATFORM) || defined(ARDUINO)
/* Arduino-ESP32's <netinet/in.h> only forward-declares struct sockaddr_in;
 * the real definition lives in lwIP's own socket header. */
#include <lwip/sockets.h>
#else
#include <netinet/in.h>
#endif
#include "dtls_lite.h"
#include "sctp_lite.h"
#include "dcep_lite.h"
#include "dtls_defrag.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RTC_SESSION_ICE,
    RTC_SESSION_DTLS_HANDSHAKE,
    RTC_SESSION_READY,
    RTC_SESSION_FAILED,
} rtc_session_state_t;

typedef void (*rtc_session_onopen_cb)(void* user);
typedef void (*rtc_session_onmessage_cb)(void* user, const uint8_t* data, size_t len);

typedef struct {
    int sockfd; /* caller-owned, bound, non-blocking, NOT connect()ed */
    struct sockaddr_in peer_addr;
    int peer_known;
    const char* local_ice_pwd; /* from our own SDP answer's a=ice-pwd */

    dtls_lite_t dtls;
    dtls_defrag_t defrag;
    sctp_lite_assoc_t sctp;
    dcep_lite_t dcep;
    rtc_session_state_t state;

    rtc_session_onopen_cb onopen;
    rtc_session_onmessage_cb onmessage;
    void* user;

    /* Time of the last datagram from anyone on this socket. Browsers send
     * STUN consent checks every few seconds while a connection is up, so
     * silence means the tab is gone (RelinkIoT tears the session down). */
    dtls_lite_now_ms_cb now_ms;
    uint32_t last_rx_ms;
} rtc_session_t;

int rtc_session_init(rtc_session_t* s, int sockfd, const char* local_ice_pwd,
                      dtls_lite_now_ms_cb now_ms,
                      rtc_session_onopen_cb onopen, rtc_session_onmessage_cb onmessage,
                      void* user);

/* Call frequently (e.g. every loop iteration). While in RTC_SESSION_ICE,
 * answers STUN Binding Requests and learns the peer address from the
 * first valid one; once learned, drives the DTLS handshake, then drains
 *+feeds decrypted SCTP packets. Keeps answering STUN (consent-freshness
 * checks) in every state, same socket. Non-blocking. */
void rtc_session_poll(rtc_session_t* s);

int rtc_session_send(rtc_session_t* s, const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif
#endif
