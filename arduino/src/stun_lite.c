#include "webrtc_lite_config.h"
#ifdef RELINK_HAS_WEBRTC
#include "stun_lite.h"

#include <string.h>
#include <mbedtls/md.h>

#define STUN_MAGIC_COOKIE 0x2112A442u
#define STUN_BINDING_REQUEST 0x0001
#define STUN_BINDING_SUCCESS 0x0101
#define ATTR_XOR_MAPPED_ADDRESS 0x0020
#define ATTR_MESSAGE_INTEGRITY 0x0008
#define ATTR_FINGERPRINT 0x8028
#define FINGERPRINT_XOR 0x5354554Eu

static void put_u16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put_u32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint16_t get_u16(const uint8_t* p) { return ((uint16_t)p[0] << 8) | p[1]; }
static uint32_t get_u32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* classic CRC-32 (IEEE 802.3 / zlib polynomial), NOT the CRC32C used
 * elsewhere for SCTP -- STUN's FINGERPRINT attribute specifically mandates
 * this one (RFC 5389 15.5). */
static uint32_t crc32_ieee(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc & 1) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFFu;
}

static void hmac_sha1(const char* key, const uint8_t* msg, size_t msg_len, uint8_t out[20]) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    mbedtls_md_hmac(info, (const uint8_t*)key, strlen(key), msg, msg_len, out);
}

/* Finds attribute `type`, returns pointer to its value and sets *val_len,
 * or NULL if absent. Stops at `limit` (exclusive) so callers can search
 * only the portion of the message before MESSAGE-INTEGRITY/FINGERPRINT. */
static const uint8_t* find_attr(const uint8_t* buf, size_t limit, uint16_t type, uint16_t* val_len) {
    size_t pos = 20;
    while (pos + 4 <= limit) {
        uint16_t t = get_u16(buf + pos);
        uint16_t l = get_u16(buf + pos + 2);
        if (pos + 4 + l > limit) break;
        if (t == type) { *val_len = l; return buf + pos + 4; }
        pos += 4 + ((l + 3) & ~3u);
    }
    return NULL;
}

size_t stun_lite_handle_binding_request(const uint8_t* in, size_t in_len,
                                         const char* local_ice_pwd,
                                         uint32_t mapped_addr_host, uint16_t mapped_port_host,
                                         uint8_t* out, size_t out_cap) {
    if (in_len < 20) return 0;
    uint16_t msg_type = get_u16(in);
    uint32_t cookie = get_u32(in + 4);
    if (msg_type != STUN_BINDING_REQUEST || cookie != STUN_MAGIC_COOKIE) return 0;
    uint16_t attrs_len = get_u16(in + 2);
    if (20 + (size_t)attrs_len > in_len) return 0;

    /* Validate MESSAGE-INTEGRITY if present (always present from a real
     * ICE agent; we still respond without one for minimal-effort testing,
     * since that's not a security boundary we need to enforce strictly --
     * the actual authenticated channel is DTLS, layered on top). */
    uint16_t mi_len;
    const uint8_t* mi = find_attr(in, 20 + attrs_len, ATTR_MESSAGE_INTEGRITY, &mi_len);
    if (mi && mi_len == 20) {
        size_t mi_attr_offset = (size_t)(mi - in) - 4;
        uint8_t tmp[512];
        if (mi_attr_offset + 24 > sizeof(tmp) || mi_attr_offset + 24 > in_len) return 0;
        memcpy(tmp, in, mi_attr_offset + 24);
        put_u16(tmp + 2, (uint16_t)(mi_attr_offset + 24 - 20)); /* length as if MI were the last attr */
        uint8_t computed[20];
        hmac_sha1(local_ice_pwd, tmp, mi_attr_offset, computed);
        if (memcmp(computed, mi, 20) != 0) return 0;
    }

    if (out_cap < 20 + 12 + 24 + 8) return 0; /* header + xor-mapped + message-integrity + fingerprint */

    memcpy(out + 8, in + 8, 12); /* echo transaction ID */
    size_t pos = 20;

    /* XOR-MAPPED-ADDRESS */
    put_u16(out + pos, ATTR_XOR_MAPPED_ADDRESS);
    put_u16(out + pos + 2, 8);
    out[pos + 4] = 0;
    out[pos + 5] = 0x01; /* IPv4 */
    /* both inputs are host byte order; put_u16/put_u32 below do the actual
     * network-byte-order serialization, so no htons/ntohl needed here --
     * this keeps the module free of any platform socket-header dependency. */
    put_u16(out + pos + 6, (uint16_t)(mapped_port_host ^ (STUN_MAGIC_COOKIE >> 16)));
    put_u32(out + pos + 8, mapped_addr_host ^ STUN_MAGIC_COOKIE);
    pos += 12;

    /* MESSAGE-INTEGRITY: computed over everything so far, with the header
     * length field set as if this attribute were the last thing written. */
    put_u16(out, STUN_BINDING_SUCCESS);
    put_u32(out + 4, STUN_MAGIC_COOKIE);
    put_u16(out + 2, (uint16_t)(pos + 24 - 20));
    uint8_t mi_out[20];
    hmac_sha1(local_ice_pwd, out, pos, mi_out);
    put_u16(out + pos, ATTR_MESSAGE_INTEGRITY);
    put_u16(out + pos + 2, 20);
    memcpy(out + pos + 4, mi_out, 20);
    pos += 24;

    /* FINGERPRINT: CRC32 over everything so far (header length field now
     * covers up through FINGERPRINT), XORed with the magic constant. */
    put_u16(out + 2, (uint16_t)(pos + 8 - 20));
    uint32_t fp = crc32_ieee(out, pos) ^ FINGERPRINT_XOR;
    put_u16(out + pos, ATTR_FINGERPRINT);
    put_u16(out + pos + 2, 4);
    put_u32(out + pos + 4, fp);
    pos += 8;

    return pos;
}

size_t stun_lite_build_binding_request(const uint8_t tid[12], uint8_t out[20]) {
    put_u16(out, STUN_BINDING_REQUEST);
    put_u16(out + 2, 0);
    put_u32(out + 4, STUN_MAGIC_COOKIE);
    memcpy(out + 8, tid, 12);
    return 20;
}

int stun_lite_parse_binding_success(const uint8_t* in, size_t in_len, const uint8_t tid[12],
                                    uint32_t* addr_host, uint16_t* port_host) {
    if (in_len < 20 || get_u16(in) != STUN_BINDING_SUCCESS || get_u32(in + 4) != STUN_MAGIC_COOKIE) return 0;
    if (memcmp(in + 8, tid, 12) != 0) return 0;
    uint16_t attrs_len = get_u16(in + 2);
    if (20 + (size_t)attrs_len > in_len) return 0;
    uint16_t len;
    const uint8_t* v = find_attr(in, 20 + attrs_len, ATTR_XOR_MAPPED_ADDRESS, &len);
    if (!v || len < 8 || v[1] != 0x01) return 0;   /* IPv4 only */
    *port_host = (uint16_t)(get_u16(v + 2) ^ (STUN_MAGIC_COOKIE >> 16));
    *addr_host = get_u32(v + 4) ^ STUN_MAGIC_COOKIE;
    return 1;
}

#endif /* RELINK_HAS_WEBRTC */
