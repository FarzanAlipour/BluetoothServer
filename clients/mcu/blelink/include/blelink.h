/*
 * BleLink v1 client (GATT central) session for microcontrollers. Portable C99.
 *
 * Implements the central side of docs/PROTOCOL.md: HELLO, message send with flow control, ACK,
 * RESUME/MSG_RESEND repair, timeouts, duplicate memory, buffered or streaming receive.
 *
 * - No malloc, no OS calls, no floating point. The context struct and all buffers belong to the caller.
 * - Not thread-safe: call every blelink_* function from the same task / context. In particular do NOT
 *   call blelink_on_packet() from the BLE host callback: copy the notification into a queue there and
 *   feed it from your main loop (see ports/).
 * - The transport adapter (the "port") provides send_packet(); it reports backpressure with
 *   BLELINK_TX_BUSY and later calls blelink_on_ready().
 *
 * Compile-time sizing (define before including, or with -D, the same for every translation unit):
 *   BLELINK_MAX_RX_WINDOW  max messages received concurrently (receive slots)      default 1
 *   BLELINK_MAX_TX         max messages queued or in flight for sending            default 2
 *   BLELINK_CTRL_QUEUE     pending control packets (ACK/RESUME/PROGRESS/CANCEL)    default 4
 *   BLELINK_DUP_MEMORY     remembered completed/rejected msgIds (>= window)         default MAX_RX_WINDOW + 1
 *   BLELINK_UNKNOWN_MEMORY msgIds answered with RESUME(id,0) (lost MSG_START)       default 2
 *   BLELINK_CRC32_IMPL     0 bitwise / 1 nibble table / 2 256-entry table          default 0
 */
#ifndef BLELINK_H
#define BLELINK_H

#include <stddef.h>
#include <stdint.h>

#include "blelink_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BLELINK_MAX_RX_WINDOW
#define BLELINK_MAX_RX_WINDOW 1
#endif
#ifndef BLELINK_MAX_TX
#define BLELINK_MAX_TX 2
#endif
#ifndef BLELINK_CTRL_QUEUE
#define BLELINK_CTRL_QUEUE 4
#endif
#ifndef BLELINK_DUP_MEMORY
#define BLELINK_DUP_MEMORY (BLELINK_MAX_RX_WINDOW + 1)
#endif
#ifndef BLELINK_UNKNOWN_MEMORY
#define BLELINK_UNKNOWN_MEMORY 2
#endif

#if BLELINK_MAX_RX_WINDOW < 1 || BLELINK_MAX_RX_WINDOW > 255
#error "BLELINK_MAX_RX_WINDOW must be 1..255"
#endif
#if BLELINK_MAX_TX < 1 || BLELINK_MAX_TX > 255
#error "BLELINK_MAX_TX must be 1..255"
#endif
#if BLELINK_DUP_MEMORY < BLELINK_MAX_RX_WINDOW
#error "BLELINK_DUP_MEMORY must be >= BLELINK_MAX_RX_WINDOW (spec 6.3)"
#endif

/* ---- Return codes ------------------------------------------------------------------------------ */
#define BLELINK_OK 0
#define BLELINK_ERR_ARG (-1)       /* bad argument / configuration */
#define BLELINK_ERR_NOT_READY (-2) /* session not READY (no server HELLO yet) */
#define BLELINK_ERR_FULL (-3)      /* no free send slot (BLELINK_MAX_TX) */
#define BLELINK_ERR_TOO_LARGE (-4) /* longer than the server's maxMessage */
#define BLELINK_ERR_UNKNOWN_ID (-5)

/* send_packet() results */
#define BLELINK_TX_OK 0   /* the stack accepted the packet */
#define BLELINK_TX_BUSY 1 /* stack queue full: nothing was sent; the port calls blelink_on_ready() later */
/* any negative value = hard error: treated like BUSY (expect a disconnect to follow) */

/* ---- Events ------------------------------------------------------------------------------------ */
typedef enum {
    BLELINK_EVT_PEER_READY = 1,   /* server HELLO received: session READY (also after a reset) */
    BLELINK_EVT_HANDSHAKE_FAILED, /* no server HELLO after 1 + 3 attempts: the port should disconnect */
    BLELINK_EVT_DISCONNECTED,     /* after blelink_on_disconnected() */
    BLELINK_EVT_SENT,             /* msg_id ACKed with status OK */
    BLELINK_EVT_FAILED,           /* msg_id failed, reason in .reason (blelink_fail_t) */
    BLELINK_EVT_PROGRESS,         /* PROGRESS from the server for msg_id: .offset = bytesReceived */
    BLELINK_EVT_RECEIVED,         /* buffered mode: complete verified message in .data/.len */
    BLELINK_EVT_RX_START,         /* streaming mode: new message msg_id, .total bytes */
    BLELINK_EVT_RX_CHUNK,         /* streaming mode: .data/.len at .offset (provisional until RX_DONE) */
    BLELINK_EVT_RX_DONE,          /* streaming mode: all bytes in, .crc_ok says whether to keep them */
    BLELINK_EVT_RX_ABORT          /* streaming mode: discard everything of msg_id (restart/cancel/reset/...) */
} blelink_event_type_t;

typedef enum {
    BLELINK_FAIL_DISCONNECTED = 1,
    BLELINK_FAIL_RESET,
    BLELINK_FAIL_TIMEOUT,
    BLELINK_FAIL_CRC,
    BLELINK_FAIL_TOO_LARGE,
    BLELINK_FAIL_BUSY,
    BLELINK_FAIL_REJECTED,
    BLELINK_FAIL_LINK_UNRELIABLE,
    BLELINK_FAIL_CANCELLED
} blelink_fail_t;

typedef struct {
    blelink_event_type_t type;
    uint16_t msg_id;
    uint8_t reason;       /* FAILED: blelink_fail_t */
    uint8_t crc_ok;       /* RX_DONE */
    uint8_t reset;        /* PEER_READY: 1 if this READY follows a session reset */
    uint32_t total;       /* RX_START, RX_DONE, RECEIVED: message length */
    uint32_t offset;      /* RX_CHUNK: offset of .data; PROGRESS: bytes received by the server */
    const uint8_t *data;  /* RECEIVED, RX_CHUNK: valid only during the callback */
    uint32_t len;         /* RECEIVED, RX_CHUNK */
} blelink_event_t;

/* Event callback. The return value matters for three events only, everything else ignores it:
 *   RX_START  non-zero -> reject the message now (ACK status 4)
 *   RECEIVED  non-zero -> ACK status 4 (rejected by the application) instead of 0
 *   RX_DONE   (crc_ok) non-zero -> ACK status 4 instead of 0
 * The callback MAY call blelink_send()/blelink_cancel(). */
typedef int (*blelink_event_cb)(void *user, const blelink_event_t *evt);

/* Transport: write one packet to the RX characteristic. with_response = 1 for HELLO (spec 1),
 * 0 for everything else (Write Without Response). Return BLELINK_TX_OK or BLELINK_TX_BUSY.
 * Must not call back into the library. */
typedef int (*blelink_send_packet_cb)(void *user, const uint8_t *data, uint16_t len, int with_response);

/* Optional clock. If NULL, the library uses the time passed to the last blelink_poll(). */
typedef uint32_t (*blelink_now_cb)(void *user);

typedef struct {
    /* Own receive limits, sent in HELLO. */
    uint16_t max_packet;  /* largest packet we accept, 20..512; usually ATT_MTU - 3 */
    uint32_t max_message; /* streaming mode: advertised maxMessage (0 = 0xFFFFFFFF, no limit).
                             buffered mode: 0 = rx_buffer_size / window; else min(this, that) */
    uint8_t window;       /* receive window 1..BLELINK_MAX_RX_WINDOW (0 = BLELINK_MAX_RX_WINDOW) */
    uint8_t timeout;      /* 0.25 s units, 0 = default (10 s); raise it if flash writes block */
    uint8_t want_progress; /* set HELLO flag bit 0: ask the server for PROGRESS packets */

    blelink_send_packet_cb send_packet;
    blelink_event_cb on_event;
    blelink_now_cb now_ms; /* optional */
    void *user;
} blelink_config_t;

typedef struct {
    /* Receive buffer, split evenly between the `window` receive slots (buffered mode).
       NULL / 0 = streaming mode: data is handed out in RX_CHUNK events, nothing is stored. */
    uint8_t *rx_buffer;
    uint32_t rx_buffer_size;
    /* Packet build buffer, at least 20 bytes; outgoing packets are never larger than this.
       Use ATT_MTU - 3 of your stack's maximum (for example 244 or 512). */
    uint8_t *tx_packet;
    uint16_t tx_packet_size;
} blelink_buffers_t;

/* ---- Internal state (exposed only so the caller can allocate it statically) -------------------- */
typedef struct {
    const uint8_t *data;
    uint32_t len, crc, offset, last_ms, restart_offset;
    uint16_t id;
    uint8_t state; /* 0 free, 1 queued, 2 sending, 3 wait ack */
    uint8_t mode;  /* next packet: 0 MSG_START, 1 MSG_DATA, 2 MSG_RESEND */
    uint8_t restart_guard, restarts, crc_retries, timeouts;
} blelink_tx_slot_t;

typedef struct {
    uint8_t *buf;
    uint32_t total, received, crc, crc_expected, last_ms;
    uint16_t id;
    uint8_t state; /* 0 free, 1 receiving, 2 resync */
    uint8_t mismatch_resumed, idle_resumes;
} blelink_rx_slot_t;

typedef struct {
    uint8_t type, status;
    uint16_t id;
    uint32_t value;
} blelink_ctrl_t;

typedef struct {
    uint16_t id;
    uint8_t status, used;
} blelink_dup_t;

typedef struct {
    uint32_t since_ms;
    uint16_t id;
    uint8_t used;
} blelink_unknown_t;

typedef struct {
    blelink_config_t cfg;
    blelink_buffers_t bufs;
    uint32_t now;
    uint32_t hello_ms;
    uint32_t rx_slot_size;
    uint32_t own_max_message;
    blelink_hello_t peer;
    uint16_t os_limit;
    uint16_t next_msg_id;
    uint8_t state; /* 0 closed, 1 opening, 2 ready */
    uint8_t window;
    uint8_t tx_seq, rx_expected;
    uint8_t hello_tries, hello_answers_pending, hello_pending, reset_pending;
    uint8_t rr;
    uint8_t ctrl_head, ctrl_count, dup_next;
    uint8_t in_pump, repump;
    uint32_t ctrl_dropped; /* diagnostics: control packets dropped because the queue was full */
    uint32_t gaps;         /* diagnostics: seq gaps seen */
    blelink_tx_slot_t tx[BLELINK_MAX_TX];
    blelink_rx_slot_t rx[BLELINK_MAX_RX_WINDOW];
    blelink_ctrl_t ctrl[BLELINK_CTRL_QUEUE];
    blelink_dup_t dup[BLELINK_DUP_MEMORY];
    blelink_unknown_t unknown[BLELINK_UNKNOWN_MEMORY];
} blelink_ctx_t;

/* ---- API --------------------------------------------------------------------------------------- */

/* Validates the configuration and resets ctx (state CLOSED). Returns BLELINK_OK or BLELINK_ERR_ARG. */
int blelink_init(blelink_ctx_t *ctx, const blelink_config_t *cfg, const blelink_buffers_t *bufs);

/* Call after the connection is up, the MTU exchange finished and TX notifications are enabled.
 * os_limit = ATT_MTU - 3. Sends HELLO (write with response) and waits for the server's HELLO
 * (event PEER_READY). Calling it again while open resets the session (in-flight sends fail RESET). */
int blelink_connect(blelink_ctx_t *ctx, uint16_t os_limit);

/* The MTU changed. Takes effect for the next packet; no reset (spec 5). */
void blelink_set_os_limit(blelink_ctx_t *ctx, uint16_t os_limit);

/* Feed one notification from TX (copied out of the BLE callback). */
void blelink_on_packet(blelink_ctx_t *ctx, const uint8_t *data, uint16_t len);

/* Drive timeouts; call every 10..100 ms (and whenever convenient). Also retries pending sends. */
void blelink_poll(blelink_ctx_t *ctx, uint32_t now_ms);

/* The stack has room again after send_packet() returned BLELINK_TX_BUSY. */
void blelink_on_ready(blelink_ctx_t *ctx);

/* The link is gone. Fails every send with DISCONNECTED, drops partial receives, emits DISCONNECTED. */
void blelink_on_disconnected(blelink_ctx_t *ctx);

/* Queue a message. `data` is NOT copied: keep it unchanged until SENT or FAILED for *id.
 * Returns BLELINK_OK and the msgId in *id (may be NULL). */
int blelink_send(blelink_ctx_t *ctx, const uint8_t *data, uint32_t len, uint16_t *id);

/* Abort a message being sent: CANCEL to the server, event FAILED(CANCELLED). */
int blelink_cancel(blelink_ctx_t *ctx, uint16_t id);

/* 1 if READY. */
int blelink_is_ready(const blelink_ctx_t *ctx);

/* packetSize currently used for client -> server packets. */
uint16_t blelink_packet_size(const blelink_ctx_t *ctx);

/* Effective timeout T in ms = max(own, server) (own only before READY). */
uint32_t blelink_timeout_ms(const blelink_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* BLELINK_H */
