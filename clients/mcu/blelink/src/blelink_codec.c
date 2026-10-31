/* BleLink v1 packet codec and CRC-32. See blelink_codec.h. */
#include "blelink_codec.h"

#include <string.h>

/* ---- CRC-32 ------------------------------------------------------------------------------------ */
#if BLELINK_CRC32_IMPL == 0

uint32_t blelink_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    while (len--) {
        unsigned k;
        crc ^= *data++;
        for (k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

#elif BLELINK_CRC32_IMPL == 1

static const uint32_t crc_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t blelink_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    while (len--) {
        crc ^= *data++;
        crc = (crc >> 4) ^ crc_nibble[crc & 0x0Fu];
        crc = (crc >> 4) ^ crc_nibble[crc & 0x0Fu];
    }
    return ~crc;
}

#elif BLELINK_CRC32_IMPL == 2

/* Generated once at first use would need RAM; a const table lives in flash. */
#define C1(c) (((c) >> 1) ^ (0xEDB88320u & (0u - ((c) & 1u))))
#define C8(c) C1(C1(C1(C1(C1(C1(C1(C1((uint32_t)(c)))))))))
#define R4(n) C8(n), C8((n) + 1), C8((n) + 2), C8((n) + 3)
#define R16(n) R4(n), R4((n) + 4), R4((n) + 8), R4((n) + 12)
#define R64(n) R16(n), R16((n) + 16), R16((n) + 32), R16((n) + 48)
static const uint32_t crc_table[256] = {R64(0), R64(64), R64(128), R64(192)};

uint32_t blelink_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    while (len--) crc = (crc >> 8) ^ crc_table[(crc ^ *data++) & 0xFFu];
    return ~crc;
}

#else
#error "BLELINK_CRC32_IMPL must be 0, 1 or 2"
#endif

/* ---- Decoder ----------------------------------------------------------------------------------- */
static void decode_hello_body(const uint8_t *p, blelink_hello_t *h)
{
    h->version = p[0];
    h->max_packet = blelink_get_u16(p + 1);
    h->max_message = blelink_get_u32(p + 3);
    h->window = p[7];
    h->flags = p[8];
    h->timeout = p[9];
}

int blelink_decode(const uint8_t *pkt, size_t len, blelink_packet_t *out)
{
    memset(out, 0, sizeof(*out));
    if (len < 2) return BLELINK_DECODE_EMPTY;
    out->type = pkt[0];
    out->seq = pkt[1];
    switch (pkt[0]) {
    case BLELINK_PKT_HELLO:
        if (len < BLELINK_HELLO_LEN) return BLELINK_DECODE_SHORT;
        decode_hello_body(pkt + 2, &out->hello);
        return BLELINK_DECODE_OK;
    case BLELINK_PKT_MSG_START:
        if (len < BLELINK_START_HDR) return BLELINK_DECODE_SHORT;
        out->msg_id = blelink_get_u16(pkt + 2);
        out->value = blelink_get_u32(pkt + 4);
        out->crc = blelink_get_u32(pkt + 8);
        out->payload = pkt + BLELINK_START_HDR;
        out->payload_len = (uint16_t)(len - BLELINK_START_HDR);
        return BLELINK_DECODE_OK;
    case BLELINK_PKT_MSG_DATA:
        if (len < BLELINK_DATA_HDR) return BLELINK_DECODE_SHORT;
        out->msg_id = blelink_get_u16(pkt + 2);
        out->payload = pkt + BLELINK_DATA_HDR;
        out->payload_len = (uint16_t)(len - BLELINK_DATA_HDR);
        return BLELINK_DECODE_OK;
    case BLELINK_PKT_MSG_RESEND:
        if (len < BLELINK_RESEND_HDR) return BLELINK_DECODE_SHORT;
        out->msg_id = blelink_get_u16(pkt + 2);
        out->value = blelink_get_u32(pkt + 4);
        out->payload = pkt + BLELINK_RESEND_HDR;
        out->payload_len = (uint16_t)(len - BLELINK_RESEND_HDR);
        return BLELINK_DECODE_OK;
    case BLELINK_PKT_ACK:
    case BLELINK_PKT_CANCEL:
        if (len < BLELINK_ACK_LEN) return BLELINK_DECODE_SHORT;
        out->msg_id = blelink_get_u16(pkt + 2);
        out->code = pkt[4];
        return BLELINK_DECODE_OK;
    case BLELINK_PKT_RESUME:
    case BLELINK_PKT_PROGRESS:
        if (len < BLELINK_RESUME_LEN) return BLELINK_DECODE_SHORT;
        out->msg_id = blelink_get_u16(pkt + 2);
        out->value = blelink_get_u32(pkt + 4);
        return BLELINK_DECODE_OK;
    default:
        return BLELINK_DECODE_UNKNOWN;
    }
}

int blelink_decode_info(const uint8_t *val, size_t len, blelink_hello_t *out)
{
    if (len < BLELINK_INFO_LEN) return BLELINK_DECODE_SHORT;
    decode_hello_body(val, out);
    return BLELINK_DECODE_OK;
}

/* ---- Encoders ---------------------------------------------------------------------------------- */
static size_t hdr_id(uint8_t *buf, uint8_t type, uint8_t seq, uint16_t id)
{
    buf[0] = type;
    buf[1] = seq;
    blelink_put_u16(buf + 2, id);
    return 4;
}

size_t blelink_encode_hello(uint8_t *buf, uint8_t seq, const blelink_hello_t *h)
{
    buf[0] = BLELINK_PKT_HELLO;
    buf[1] = seq;
    buf[2] = h->version;
    blelink_put_u16(buf + 3, h->max_packet);
    blelink_put_u32(buf + 5, h->max_message);
    buf[9] = h->window;
    buf[10] = h->flags;
    buf[11] = h->timeout;
    return BLELINK_HELLO_LEN;
}

size_t blelink_encode_start(uint8_t *buf, uint8_t seq, uint16_t id, uint32_t total, uint32_t crc,
                            const uint8_t *payload, uint16_t n)
{
    hdr_id(buf, BLELINK_PKT_MSG_START, seq, id);
    blelink_put_u32(buf + 4, total);
    blelink_put_u32(buf + 8, crc);
    if (n) memcpy(buf + BLELINK_START_HDR, payload, n);
    return BLELINK_START_HDR + (size_t)n;
}

size_t blelink_encode_data(uint8_t *buf, uint8_t seq, uint16_t id, const uint8_t *payload, uint16_t n)
{
    hdr_id(buf, BLELINK_PKT_MSG_DATA, seq, id);
    if (n) memcpy(buf + BLELINK_DATA_HDR, payload, n);
    return BLELINK_DATA_HDR + (size_t)n;
}

size_t blelink_encode_resend(uint8_t *buf, uint8_t seq, uint16_t id, uint32_t offset, const uint8_t *payload,
                             uint16_t n)
{
    hdr_id(buf, BLELINK_PKT_MSG_RESEND, seq, id);
    blelink_put_u32(buf + 4, offset);
    if (n) memcpy(buf + BLELINK_RESEND_HDR, payload, n);
    return BLELINK_RESEND_HDR + (size_t)n;
}

size_t blelink_encode_ack(uint8_t *buf, uint8_t seq, uint16_t id, uint8_t status)
{
    hdr_id(buf, BLELINK_PKT_ACK, seq, id);
    buf[4] = status;
    return BLELINK_ACK_LEN;
}

size_t blelink_encode_resume(uint8_t *buf, uint8_t seq, uint16_t id, uint32_t offset)
{
    hdr_id(buf, BLELINK_PKT_RESUME, seq, id);
    blelink_put_u32(buf + 4, offset);
    return BLELINK_RESUME_LEN;
}

size_t blelink_encode_cancel(uint8_t *buf, uint8_t seq, uint16_t id, uint8_t reason)
{
    hdr_id(buf, BLELINK_PKT_CANCEL, seq, id);
    buf[4] = reason;
    return BLELINK_CANCEL_LEN;
}

size_t blelink_encode_progress(uint8_t *buf, uint8_t seq, uint16_t id, uint32_t bytes)
{
    hdr_id(buf, BLELINK_PKT_PROGRESS, seq, id);
    blelink_put_u32(buf + 4, bytes);
    return BLELINK_PROGRESS_LEN;
}
