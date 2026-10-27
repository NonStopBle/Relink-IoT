#ifndef DCEP_LITE_H
#define DCEP_LITE_H
/* Minimal RFC 8832 DCEP: handles exactly the responder side of a single
 * data channel. We never originate DATA_CHANNEL_OPEN (browser JS always
 * opens the channel in the common negotiated:false case), so this only
 * needs to answer one with DATA_CHANNEL_ACK. */
#include "sctp_lite.h"

#define DCEP_PPID 50
#define WEBRTC_PPID_BINARY 53

#ifdef __cplusplus
extern "C" {
#endif

/* label: the channel's name from the DCEP OPEN (not NUL-terminated). */
typedef void (*dcep_lite_onopen_cb)(void* user, uint16_t stream_id, const uint8_t* label, size_t label_len);
typedef void (*dcep_lite_onmessage_cb)(void* user, uint16_t stream_id,
                                        const uint8_t* data, size_t len);

typedef struct {
    sctp_lite_assoc_t* assoc;
    dcep_lite_onopen_cb onopen;
    dcep_lite_onmessage_cb onmessage;
    void* user;
} dcep_lite_t;

void dcep_lite_init(dcep_lite_t* d, sctp_lite_assoc_t* assoc,
                     dcep_lite_onopen_cb onopen,
                     dcep_lite_onmessage_cb onmessage, void* user);

/* Wire this as the sctp_lite_assoc_t's deliver_cb (with d as user),
 * or call it manually from your own deliver_cb -- it demuxes DCEP control
 * messages (ppid 50) from application data and forwards the latter via
 * onmessage. */
void dcep_lite_on_sctp_deliver(void* user, uint16_t stream_id, uint32_t ppid,
                                const uint8_t* payload, size_t len);

int dcep_lite_send(dcep_lite_t* d, uint16_t stream_id, const uint8_t* data, size_t len);
int dcep_lite_send2(dcep_lite_t* d, uint16_t stream_id, const uint8_t* hdr, size_t hlen,
                     const uint8_t* body, size_t blen);

#ifdef __cplusplus
}
#endif
#endif
