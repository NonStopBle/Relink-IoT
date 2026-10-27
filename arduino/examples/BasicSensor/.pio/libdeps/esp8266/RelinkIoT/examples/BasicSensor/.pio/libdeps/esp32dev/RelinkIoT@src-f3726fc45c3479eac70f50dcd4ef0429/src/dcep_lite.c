#include "dcep_lite.h"
#include "webrtc_lite_config.h"
#ifdef RELINK_HAS_WEBRTC

#include <string.h>

#define DCEP_MSG_OPEN 0x03
#define DCEP_MSG_ACK  0x02
#define DCEP_CHANNEL_PARTIAL_RELIABLE_REXMIT_UNORDERED 0x81

static void put_u16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put_u32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

void dcep_lite_init(dcep_lite_t* d, sctp_lite_assoc_t* assoc,
                     dcep_lite_onopen_cb onopen,
                     dcep_lite_onmessage_cb onmessage, void* user) {
    memset(d, 0, sizeof(*d));
    d->assoc = assoc;
    d->onopen = onopen;
    d->onmessage = onmessage;
    d->user = user;
}

static void send_ack(dcep_lite_t* d, uint16_t stream_id) {
    uint8_t msg = DCEP_MSG_ACK;
    sctp_lite_send(d->assoc, stream_id, DCEP_PPID, &msg, 1);
}

/* 'user' here is expected to be a dcep_lite_t*, passed as the sctp_lite
 * assoc's user pointer when the whole stack is wired together. */
void dcep_lite_on_sctp_deliver(void* user, uint16_t stream_id, uint32_t ppid,
                                const uint8_t* payload, size_t len) {
    dcep_lite_t* d = (dcep_lite_t*)user;
    if (ppid == DCEP_PPID) {
        if (len >= 1 && payload[0] == DCEP_MSG_OPEN) {
            send_ack(d, stream_id);
            if (d->onopen) d->onopen(d->user, stream_id);
        }
        /* DCEP_MSG_ACK incoming: we never send OPEN ourselves, so this
         * shouldn't happen in the negotiated:false flow -- ignored. */
        return;
    }
    if (d->onmessage) d->onmessage(d->user, stream_id, payload, len);
}

int dcep_lite_send(dcep_lite_t* d, uint16_t stream_id, const uint8_t* data, size_t len) {
    return sctp_lite_send(d->assoc, stream_id, WEBRTC_PPID_BINARY, data, len);
}

#endif /* RELINK_HAS_WEBRTC */
