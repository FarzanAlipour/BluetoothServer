/*
 * BleLink v1 packet codec and CRC-32 (docs/PROTOCOL.md sections 2 and 4).
 *
 * Pure functions, no state, no Bluetooth: usable on its own (for example by a test tool).
 * All multi-byte fields are little-endian; the codec reads and writes them byte by byte, so it is
 * independent of the CPU byte order and of alignment.
 */
#ifndef BLELINK_CODEC_H
#define BLELINK_CODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- CRC-32/IEEE 802.3 (zlib) ------------------------------------------------------------------
 * Variant selected at compile time with BLELINK_CRC32_IMPL:
 *   0 = bitwise, no table (default; smallest flash, ~8 cycles per bit)
 *   1 = 16-entry nibble table (64 bytes of const data)
 *   2 = 256-entry table (1 KiB of const data, fastest)
 */
#ifndef BLELINK_CRC32_IMPL
#define BLELINK_CRC32_IMPL 0
#endif

/* zlib-style incremental CRC: start with crc = 0, feed chunks, the return value after the last chunk
 * is the final CRC. blelink_crc32(blelink_crc32(0, a, n), b, m) == crc32(a || b). */
uint32_t blelink_crc32(uint32_t crc, const uint8_t *data, size_t len);

/* ---- Constants --------------------------------------------------------------------------------- */
#define BLELINK_VERSION 1u

#define BLELINK_PKT_HELLO 0x01u
#define BLELINK_PKT_MSG_START 0x02u
#define BLELINK_PKT_MSG_DATA 0x03u
#define BLELINK_PKT_ACK 0x04u
#define BLELINK_PKT_RESUME 0x05u
#define BLELINK_PKT_CANCEL 0x06u
#define BLELINK_PKT_PROGRESS 0x07u
#define BLELINK_PKT_MSG_RESEND 0x08u

/* Fixed sizes (header included). */
#define BLELINK_HELLO_LEN 12u
#define BLELINK_START_HDR 12u
#define BLELINK_DATA_HDR 4u
#define BLELINK_RESEND_HDR 8u
#define BLELINK_ACK_LEN 5u
#define BLELINK_RESUME_LEN 8u
#define BLELINK_CANCEL_LEN 5u
#define BLELINK_PROGRESS_LEN 8u
#define BLELINK_INFO_LEN 10u /* INFO characteristic: HELLO body without type and seq */

#define BLELINK_MIN_PACKET 20u
#define BLELINK_MAX_PACKET 512u

#define BLELINK_HELLO_FLAG_PROGRESS 0x01u
#define BLELINK_HELLO_FLAG_STREAMING 0x02u

/* ACK status (section 6.3). */
#define BLELINK_ACK_OK 0u
#define BLELINK_ACK_CRC 1u
#define BLELINK_ACK_TOO_LARGE 2u
#define BLELINK_ACK_BUSY 3u
#define BLELINK_ACK_REJECTED 4u

/* CANCEL reason (section 16). */
#define BLELINK_CANCEL_APP 0u
#define BLELINK_CANCEL_GAVE_UP 1u
#define BLELINK_CANCEL_SHUTDOWN 2u

/* ---- Little-endian helpers --------------------------------------------------------------------- */
static inline uint16_t blelink_get_u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static inline uint32_t blelink_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void blelink_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static inline void blelink_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ---- Decoded packet ---------------------------------------------------------------------------- */
typedef struct {
    uint8_t version;
    uint16_t max_packet; /* raw value as sent (not clamped) */
    uint32_t max_message;
    uint8_t window;
    uint8_t flags;
    uint8_t timeout; /* 0.25 s units, 0 = default 40 */
} blelink_hello_t;

typedef struct {
    uint8_t type;
    uint8_t seq;
    uint16_t msg_id;
    uint32_t value;  /* MSG_START totalLen, RESUME/MSG_RESEND offset, PROGRESS bytesReceived */
    uint32_t crc;    /* MSG_START crc32 */
    uint8_t code;    /* ACK status, CANCEL reason */
    blelink_hello_t hello;
    const uint8_t *payload; /* MSG_START / MSG_DATA / MSG_RESEND payload (points into the packet) */
    uint16_t payload_len;
} blelink_packet_t;

#define BLELINK_DECODE_OK 0
#define BLELINK_DECODE_SHORT (-1)   /* shorter than the fixed layout: ignore (seq valid if len >= 2) */
#define BLELINK_DECODE_UNKNOWN (-2) /* unknown type: ignore, but seq counts */
#define BLELINK_DECODE_EMPTY (-3)   /* fewer than 2 bytes: not a packet at all */

/* Parses one packet. Trailing bytes after fixed-size packets are ignored (section 2.3). On SHORT and
 * UNKNOWN out->type and out->seq are still filled in. */
int blelink_decode(const uint8_t *pkt, size_t len, blelink_packet_t *out);

/* Parses the 10-byte INFO characteristic value. Returns 0 or BLELINK_DECODE_SHORT. */
int blelink_decode_info(const uint8_t *val, size_t len, blelink_hello_t *out);

/* ---- Encoders: write into buf, return the packet length. buf must be large enough. -------------- */
size_t blelink_encode_hello(uint8_t *buf, uint8_t seq, const blelink_hello_t *h);
size_t blelink_encode_start(uint8_t *buf, uint8_t seq, uint16_t id, uint32_t total, uint32_t crc,
                            const uint8_t *payload, uint16_t n);
size_t blelink_encode_data(uint8_t *buf, uint8_t seq, uint16_t id, const uint8_t *payload, uint16_t n);
size_t blelink_encode_resend(uint8_t *buf, uint8_t seq, uint16_t id, uint32_t offset, const uint8_t *payload,
                             uint16_t n);
size_t blelink_encode_ack(uint8_t *buf, uint8_t seq, uint16_t id, uint8_t status);
size_t blelink_encode_resume(uint8_t *buf, uint8_t seq, uint16_t id, uint32_t offset);
size_t blelink_encode_cancel(uint8_t *buf, uint8_t seq, uint16_t id, uint8_t reason);
size_t blelink_encode_progress(uint8_t *buf, uint8_t seq, uint16_t id, uint32_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* BLELINK_CODEC_H */
