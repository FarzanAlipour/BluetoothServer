/*
 * BleLink v1 client session (central side of docs/PROTOCOL.md). See blelink.h.
 * Section numbers in comments refer to docs/PROTOCOL.md.
 */
#include "blelink.h"

#include <string.h>

enum { ST_CLOSED = 0, ST_OPENING = 1, ST_READY = 2 };
enum { TX_FREE = 0, TX_QUEUED = 1, TX_SENDING = 2, TX_WAIT = 3 };
enum { RX_FREE = 0, RX_RECV = 1, RX_RESYNC = 2 };
enum { MODE_START = 0, MODE_DATA = 1, MODE_RESEND = 2 };

#define MAX_RESENDS 3      /* ACK timeout and CRC resends (sections 6.3, 10) */
#define MAX_RESTARTS 8     /* RESUME restarts per message (8.2) */
#define MAX_IDLE_RESUMES 3 /* receiver idle timeout (10) */
#define HELLO_RETRIES 3    /* HELLO timeout (10) */
#define DEFAULT_TIMEOUT_UNITS 40u
#define PROGRESS_STEP_SHIFT 16 /* PROGRESS every 65536 bytes (16) */

/* ---- small helpers ----------------------------------------------------------------------------- */
static uint32_t now_ms(blelink_ctx_t *ctx)
{
    if (ctx->cfg.now_ms) ctx->now = ctx->cfg.now_ms(ctx->cfg.user);
    return ctx->now;
}

static int elapsed(uint32_t since, uint32_t now, uint32_t span) { return (uint32_t)(now - since) >= span; }

static uint32_t units(uint8_t t) { return t ? t : DEFAULT_TIMEOUT_UNITS; }

static uint16_t clamp_packet(uint32_t v)
{
    if (v < BLELINK_MIN_PACKET) return BLELINK_MIN_PACKET;
    if (v > BLELINK_MAX_PACKET) return BLELINK_MAX_PACKET;
    return (uint16_t)v;
}

static int is_streaming(const blelink_ctx_t *ctx) { return ctx->bufs.rx_buffer == NULL; }

static int emit(blelink_ctx_t *ctx, const blelink_event_t *e)
{
    return ctx->cfg.on_event ? ctx->cfg.on_event(ctx->cfg.user, e) : 0;
}

static int emit_simple(blelink_ctx_t *ctx, blelink_event_type_t type, uint16_t id)
{
    blelink_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.msg_id = id;
    return emit(ctx, &e);
}

static void own_hello(const blelink_ctx_t *ctx, blelink_hello_t *h)
{
    h->version = BLELINK_VERSION;
    h->max_packet = clamp_packet(ctx->cfg.max_packet);
    h->max_message = ctx->own_max_message;
    h->window = ctx->window;
    h->flags = (uint8_t)((ctx->cfg.want_progress ? BLELINK_HELLO_FLAG_PROGRESS : 0u) |
                         (is_streaming(ctx) ? BLELINK_HELLO_FLAG_STREAMING : 0u));
    h->timeout = ctx->cfg.timeout;
}

uint32_t blelink_timeout_ms(const blelink_ctx_t *ctx)
{
    uint32_t u = units(ctx->cfg.timeout);
    if (ctx->state == ST_READY && units(ctx->peer.timeout) > u) u = units(ctx->peer.timeout);
    return u * 250u;
}

uint16_t blelink_packet_size(const blelink_ctx_t *ctx)
{
    uint32_t ps = BLELINK_MIN_PACKET; /* before HELLO completes (5) */
    if (ctx->state == ST_READY) {
        uint32_t peer_max = clamp_packet(ctx->peer.max_packet);
        ps = clamp_packet(peer_max < ctx->os_limit ? peer_max : ctx->os_limit);
    }
    if (ps > ctx->bufs.tx_packet_size) ps = ctx->bufs.tx_packet_size;
    return (uint16_t)ps;
}

int blelink_is_ready(const blelink_ctx_t *ctx) { return ctx->state == ST_READY; }

/* ---- control packet queue ---------------------------------------------------------------------- */
static void ctrl_push(blelink_ctx_t *ctx, uint8_t type, uint16_t id, uint8_t status, uint32_t value)
{
    blelink_ctrl_t *c;
    if (ctx->ctrl_count >= BLELINK_CTRL_QUEUE) {
        /* Recoverable by protocol timers: lost ACK -> sender resend, lost RESUME -> idle timeout. */
        ctx->ctrl_dropped++;
        return;
    }
    c = &ctx->ctrl[(ctx->ctrl_head + ctx->ctrl_count) % BLELINK_CTRL_QUEUE];
    c->type = type;
    c->id = id;
    c->status = status;
    c->value = value;
    ctx->ctrl_count++;
}

/* ---- duplicate memory (completed msgIds, 6.3) and silent memory -------------------------------- */
static int dup_find(const blelink_ctx_t *ctx, uint16_t id, uint8_t *status)
{
    unsigned i;
    for (i = 0; i < BLELINK_DUP_MEMORY; i++) {
        if (ctx->dup[i].used && ctx->dup[i].id == id) {
            *status = ctx->dup[i].status;
            return 1;
        }
    }
    return 0;
}

static void dup_add(blelink_ctx_t *ctx, uint16_t id, uint8_t status)
{
    blelink_dup_t *d = &ctx->dup[ctx->dup_next];
    d->id = id;
    d->status = status;
    d->used = 1;
    ctx->dup_next = (uint8_t)((ctx->dup_next + 1u) % BLELINK_DUP_MEMORY);
}

/* "Silent" ids: packets for them are ignored without RESUME until a MSG_START for the id arrives or
 * the entry ages out after T. Used for: ids already answered with RESUME(id,0) (8.1), ids rejected
 * by an early ACK (6.3), ids that failed CRC (their retry re-uses the id, so they must NOT go into
 * the duplicate memory), and cancelled ids. */
static int silent_has(const blelink_ctx_t *ctx, uint16_t id)
{
    unsigned i;
    for (i = 0; i < BLELINK_UNKNOWN_MEMORY; i++)
        if (ctx->unknown[i].used && ctx->unknown[i].id == id) return 1;
    return 0;
}

static void silent_del(blelink_ctx_t *ctx, uint16_t id)
{
    unsigned i;
    for (i = 0; i < BLELINK_UNKNOWN_MEMORY; i++)
        if (ctx->unknown[i].used && ctx->unknown[i].id == id) ctx->unknown[i].used = 0;
}

static void silent_add(blelink_ctx_t *ctx, uint16_t id)
{
    unsigned i, pick = 0;
    for (i = 0; i < BLELINK_UNKNOWN_MEMORY; i++) {
        if (!ctx->unknown[i].used) {
            pick = i;
            break;
        }
        if ((uint32_t)(ctx->now - ctx->unknown[i].since_ms) > (uint32_t)(ctx->now - ctx->unknown[pick].since_ms))
            pick = i; /* oldest */
    }
    ctx->unknown[pick].id = id;
    ctx->unknown[pick].since_ms = ctx->now;
    ctx->unknown[pick].used = 1;
}

/* ---- send side --------------------------------------------------------------------------------- */
static blelink_tx_slot_t *tx_find(blelink_ctx_t *ctx, uint16_t id, int started_only)
{
    unsigned i;
    for (i = 0; i < BLELINK_MAX_TX; i++) {
        blelink_tx_slot_t *s = &ctx->tx[i];
        if (s->state == TX_FREE || s->id != id) continue;
        if (started_only && s->state == TX_QUEUED) continue;
        return s;
    }
    return NULL;
}

static void tx_restart(blelink_tx_slot_t *s, uint32_t offset)
{
    s->offset = offset;
    s->mode = offset ? MODE_RESEND : MODE_START;
    s->state = TX_SENDING;
}

static void tx_fail(blelink_ctx_t *ctx, blelink_tx_slot_t *s, blelink_fail_t reason, int send_cancel,
                    uint8_t cancel_reason)
{
    blelink_event_t e;
    uint16_t id = s->id;
    if (send_cancel && ctx->state == ST_READY) ctrl_push(ctx, BLELINK_PKT_CANCEL, id, cancel_reason, 0);
    memset(s, 0, sizeof(*s));
    memset(&e, 0, sizeof(e));
    e.type = BLELINK_EVT_FAILED;
    e.msg_id = id;
    e.reason = (uint8_t)reason;
    emit(ctx, &e);
}

static void tx_activate(blelink_ctx_t *ctx)
{
    unsigned i, inflight = 0;
    unsigned window = ctx->peer.window ? ctx->peer.window : 1u;
    for (i = 0; i < BLELINK_MAX_TX; i++)
        if (ctx->tx[i].state == TX_SENDING || ctx->tx[i].state == TX_WAIT) inflight++;
    while (inflight < window) {
        blelink_tx_slot_t *oldest = NULL;
        for (i = 0; i < BLELINK_MAX_TX; i++) {
            blelink_tx_slot_t *s = &ctx->tx[i];
            if (s->state != TX_QUEUED) continue;
            /* oldest = largest distance behind next_msg_id (ids are assigned in increasing order, 7) */
            if (!oldest || (uint16_t)(ctx->next_msg_id - s->id) > (uint16_t)(ctx->next_msg_id - oldest->id))
                oldest = s;
        }
        if (!oldest) break;
        oldest->state = TX_SENDING;
        inflight++;
    }
}

static int transmit(blelink_ctx_t *ctx, size_t n, int with_response)
{
    return ctx->cfg.send_packet(ctx->cfg.user, ctx->bufs.tx_packet, (uint16_t)n, with_response) == BLELINK_TX_OK;
}

/* Builds the next packet of message s and tries to send it. Returns 1 if the stack took it. */
static int tx_send_next(blelink_ctx_t *ctx, blelink_tx_slot_t *s)
{
    uint8_t *b = ctx->bufs.tx_packet;
    uint16_t ps = blelink_packet_size(ctx);
    uint32_t left, cap;
    uint16_t n;
    size_t len;
    if (s->mode == MODE_START) {
        s->offset = 0;
        cap = ps - BLELINK_START_HDR;
    } else if (s->mode == MODE_RESEND) {
        cap = ps - BLELINK_RESEND_HDR;
    } else {
        cap = ps - BLELINK_DATA_HDR;
    }
    left = s->len - s->offset;
    n = (uint16_t)(left < cap ? left : cap);
    if (s->mode == MODE_START)
        len = blelink_encode_start(b, ctx->tx_seq, s->id, s->len, s->crc, s->data, n);
    else if (s->mode == MODE_RESEND)
        len = blelink_encode_resend(b, ctx->tx_seq, s->id, s->offset, s->data + s->offset, n);
    else
        len = blelink_encode_data(b, ctx->tx_seq, s->id, s->data + s->offset, n);
    if (!transmit(ctx, len, 0)) return 0;
    ctx->tx_seq++;
    s->offset += n;
    s->mode = MODE_DATA;
    s->restart_guard = 0; /* first packet after a restart went out (8.2) */
    s->last_ms = ctx->now;
    if (s->offset >= s->len) s->state = TX_WAIT; /* ACK timer starts now (10) */
    return 1;
}

/* One packet: HELLO first, then control packets, then message data round-robin. 1 = sent. */
static int pump_one(blelink_ctx_t *ctx)
{
    uint8_t *b = ctx->bufs.tx_packet;
    unsigned k;
    if (ctx->state == ST_CLOSED) return 0;
    if (ctx->hello_pending) {
        blelink_hello_t h;
        own_hello(ctx, &h);
        if (!transmit(ctx, blelink_encode_hello(b, 0, &h), 1)) return 0;
        ctx->hello_pending = 0;
        ctx->tx_seq = 1; /* HELLO is seq 0 and restarts the counter (2.1) */
        ctx->hello_ms = ctx->now;
        return 1;
    }
    if (ctx->state != ST_READY) return 0;
    if (ctx->ctrl_count) {
        const blelink_ctrl_t *c = &ctx->ctrl[ctx->ctrl_head];
        size_t len;
        switch (c->type) {
        case BLELINK_PKT_ACK: len = blelink_encode_ack(b, ctx->tx_seq, c->id, c->status); break;
        case BLELINK_PKT_RESUME: len = blelink_encode_resume(b, ctx->tx_seq, c->id, c->value); break;
        case BLELINK_PKT_CANCEL: len = blelink_encode_cancel(b, ctx->tx_seq, c->id, c->status); break;
        default: len = blelink_encode_progress(b, ctx->tx_seq, c->id, c->value); break;
        }
        if (!transmit(ctx, len, 0)) return 0;
        ctx->tx_seq++;
        ctx->ctrl_head = (uint8_t)((ctx->ctrl_head + 1u) % BLELINK_CTRL_QUEUE);
        ctx->ctrl_count--;
        return 1;
    }
    tx_activate(ctx);
    for (k = 1; k <= BLELINK_MAX_TX; k++) {
        unsigned i = (ctx->rr + k) % BLELINK_MAX_TX;
        if (ctx->tx[i].state != TX_SENDING) continue;
        if (!tx_send_next(ctx, &ctx->tx[i])) return 0;
        ctx->rr = (uint8_t)i;
        return 1;
    }
    return 0;
}

static void pump(blelink_ctx_t *ctx)
{
    if (ctx->in_pump) {
        ctx->repump = 1;
        return;
    }
    ctx->in_pump = 1;
    do {
        ctx->repump = 0;
        while (pump_one(ctx)) {
        }
    } while (ctx->repump);
    ctx->in_pump = 0;
}

static void lock(blelink_ctx_t *ctx) { ctx->in_pump++; }

static void unlock_and_pump(blelink_ctx_t *ctx)
{
    ctx->in_pump--;
    if (ctx->in_pump == 0) pump(ctx);
}

/* ---- receive side ------------------------------------------------------------------------------ */
static blelink_rx_slot_t *rx_find(blelink_ctx_t *ctx, uint16_t id)
{
    unsigned i;
    for (i = 0; i < ctx->window; i++)
        if (ctx->rx[i].state != RX_FREE && ctx->rx[i].id == id) return &ctx->rx[i];
    return NULL;
}

/* Drops a partial message (streaming: tells the application to discard what it got). */
static void rx_drop(blelink_ctx_t *ctx, blelink_rx_slot_t *r)
{
    uint16_t id = r->id;
    uint8_t *buf = r->buf;
    memset(r, 0, sizeof(*r));
    r->buf = buf;
    if (is_streaming(ctx)) emit_simple(ctx, BLELINK_EVT_RX_ABORT, id);
}

/* Early/negative ACK: the following packets of the id are ignored silently (6.3). */
static void rx_reject(blelink_ctx_t *ctx, uint16_t id, uint8_t status)
{
    silent_add(ctx, id);
    ctrl_push(ctx, BLELINK_PKT_ACK, id, status, 0);
}

static void rx_complete(blelink_ctx_t *ctx, blelink_rx_slot_t *r)
{
    blelink_event_t e;
    uint16_t id = r->id;
    int ok = r->crc == r->crc_expected;
    int app = 0;
    memset(&e, 0, sizeof(e));
    e.msg_id = id;
    e.total = r->total;
    if (is_streaming(ctx)) {
        e.type = BLELINK_EVT_RX_DONE;
        e.crc_ok = (uint8_t)ok;
        app = emit(ctx, &e);
    } else if (ok) {
        e.type = BLELINK_EVT_RECEIVED;
        e.data = r->buf;
        e.len = r->total;
        app = emit(ctx, &e);
    }
    {
        uint8_t *buf = r->buf;
        memset(r, 0, sizeof(*r));
        r->buf = buf;
    }
    if (!ok) {
        /* not remembered as done: the sender retries with the same msgId (6.3 status 1) */
        rx_reject(ctx, id, BLELINK_ACK_CRC);
        return;
    }
    {
        uint8_t status = app ? BLELINK_ACK_REJECTED : BLELINK_ACK_OK;
        dup_add(ctx, id, status);
        ctrl_push(ctx, BLELINK_PKT_ACK, id, status, 0);
    }
}

static void rx_accept(blelink_ctx_t *ctx, blelink_rx_slot_t *r, const uint8_t *p, uint16_t n)
{
    if ((uint32_t)n > r->total - r->received) { /* corrupt sender (6.3): fail with status 1 */
        uint16_t id = r->id;
        rx_drop(ctx, r);
        rx_reject(ctx, id, BLELINK_ACK_CRC);
        return;
    }
    r->last_ms = ctx->now;
    if (n) {
        uint32_t before = r->received;
        if (is_streaming(ctx)) {
            blelink_event_t e;
            memset(&e, 0, sizeof(e));
            e.type = BLELINK_EVT_RX_CHUNK;
            e.msg_id = r->id;
            e.total = r->total;
            e.offset = before;
            e.data = p;
            e.len = n;
            emit(ctx, &e);
        } else {
            memcpy(r->buf + before, p, n);
        }
        r->crc = blelink_crc32(r->crc, p, n);
        r->received = before + n;
        r->idle_resumes = 0;
        if ((ctx->peer.flags & BLELINK_HELLO_FLAG_PROGRESS) && r->received < r->total &&
            (before >> PROGRESS_STEP_SHIFT) != (r->received >> PROGRESS_STEP_SHIFT))
            ctrl_push(ctx, BLELINK_PKT_PROGRESS, r->id, 0, r->received);
    }
    if (r->received == r->total) rx_complete(ctx, r);
}

static void rx_start(blelink_ctx_t *ctx, const blelink_packet_t *pk)
{
    blelink_rx_slot_t *r;
    uint8_t status;
    unsigned i;
    if (dup_find(ctx, pk->msg_id, &status)) { /* retry after a lost ACK: re-ACK, do not deliver (6.3) */
        ctrl_push(ctx, BLELINK_PKT_ACK, pk->msg_id, status, 0);
        return;
    }
    silent_del(ctx, pk->msg_id);
    r = rx_find(ctx, pk->msg_id);
    if (r) rx_drop(ctx, r); /* same id while receiving: restart (8.2) */
    if (pk->value > ctx->own_max_message) {
        rx_reject(ctx, pk->msg_id, BLELINK_ACK_TOO_LARGE);
        return;
    }
    if (!r) {
        for (i = 0; i < ctx->window; i++)
            if (ctx->rx[i].state == RX_FREE) {
                r = &ctx->rx[i];
                break;
            }
        if (!r) { /* window exceeded (6.3) */
            rx_reject(ctx, pk->msg_id, BLELINK_ACK_BUSY);
            return;
        }
    }
    r->id = pk->msg_id;
    r->total = pk->value;
    r->received = 0;
    r->crc = 0;
    r->crc_expected = pk->crc;
    r->state = RX_RECV;
    r->mismatch_resumed = 0;
    r->idle_resumes = 0;
    r->last_ms = ctx->now;
    if (is_streaming(ctx)) {
        blelink_event_t e;
        memset(&e, 0, sizeof(e));
        e.type = BLELINK_EVT_RX_START;
        e.msg_id = r->id;
        e.total = r->total;
        if (emit(ctx, &e)) {
            uint8_t *buf = r->buf;
            memset(r, 0, sizeof(*r));
            r->buf = buf;
            rx_reject(ctx, pk->msg_id, BLELINK_ACK_REJECTED);
            return;
        }
    }
    rx_accept(ctx, r, pk->payload, pk->payload_len);
}

/* MSG_DATA / MSG_RESEND for an id we are not receiving (8.1). */
static void rx_unknown(blelink_ctx_t *ctx, uint16_t id)
{
    uint8_t status;
    if (dup_find(ctx, id, &status) || silent_has(ctx, id)) return;
    silent_add(ctx, id);
    ctrl_push(ctx, BLELINK_PKT_RESUME, id, 0, 0);
}

static void rx_gap(blelink_ctx_t *ctx)
{
    unsigned i;
    ctx->gaps++;
    for (i = 0; i < ctx->window; i++) { /* every incomplete message resyncs (8.1, D2) */
        blelink_rx_slot_t *r = &ctx->rx[i];
        if (r->state == RX_FREE) continue;
        ctrl_push(ctx, BLELINK_PKT_RESUME, r->id, 0, r->received);
        r->state = RX_RESYNC;
        r->mismatch_resumed = 0;
        r->last_ms = ctx->now;
    }
}

/* ---- session ----------------------------------------------------------------------------------- */
/* Fails started messages (and queued ones if fail_queued), drops partial receives and all
 * per-session memory. Does not touch state or seq counters. */
static void session_clear(blelink_ctx_t *ctx, blelink_fail_t reason, int fail_queued)
{
    unsigned i;
    ctx->ctrl_count = 0;
    ctx->ctrl_head = 0;
    memset(ctx->dup, 0, sizeof(ctx->dup));
    memset(ctx->unknown, 0, sizeof(ctx->unknown));
    ctx->dup_next = 0;
    for (i = 0; i < ctx->window; i++)
        if (ctx->rx[i].state != RX_FREE) rx_drop(ctx, &ctx->rx[i]);
    for (i = 0; i < BLELINK_MAX_TX; i++) {
        blelink_tx_slot_t *s = &ctx->tx[i];
        if (s->state == TX_FREE) continue;
        if (s->state == TX_QUEUED && !fail_queued) continue;
        tx_fail(ctx, s, reason, 0, 0);
    }
}

static void on_hello(blelink_ctx_t *ctx, const blelink_hello_t *h)
{
    blelink_event_t e;
    if (ctx->state == ST_CLOSED || h->version == 0) return; /* version 0 does not exist (12) */
    ctx->peer = *h;
    if (ctx->peer.window == 0) ctx->peer.window = 1; /* MUST be >= 1 (4.1) */
    ctx->rx_expected = 1;                            /* the peer restarted its seq counter */
    memset(&e, 0, sizeof(e));
    e.type = BLELINK_EVT_PEER_READY;
    if (ctx->state == ST_OPENING) {
        if (ctx->hello_answers_pending) ctx->hello_answers_pending--;
        ctx->state = ST_READY;
        e.reset = ctx->reset_pending;
        ctx->reset_pending = 0;
        emit(ctx, &e);
        return;
    }
    e.reset = 1;
    if (ctx->hello_answers_pending) {
        /* Late answer to a HELLO we re-sent after a HELLO timeout: the server reset its session once
         * more, but answering would start a HELLO ping-pong (see NOTES.md, spec issue S1). Drop the
         * session state like a reset, keep our seq counter, do not answer. */
        ctx->hello_answers_pending--;
        session_clear(ctx, BLELINK_FAIL_RESET, 0);
        emit(ctx, &e);
        return;
    }
    /* HELLO while READY: the server resets the session; we answer with our HELLO (3). */
    session_clear(ctx, BLELINK_FAIL_RESET, 0);
    ctx->hello_pending = 1;
    emit(ctx, &e);
}

int blelink_init(blelink_ctx_t *ctx, const blelink_config_t *cfg, const blelink_buffers_t *bufs)
{
    unsigned i;
    uint8_t window;
    if (!ctx || !cfg || !bufs || !cfg->send_packet || !bufs->tx_packet || bufs->tx_packet_size < BLELINK_MIN_PACKET)
        return BLELINK_ERR_ARG;
    window = cfg->window ? cfg->window : (uint8_t)BLELINK_MAX_RX_WINDOW;
    if (window > BLELINK_MAX_RX_WINDOW) return BLELINK_ERR_ARG;
    if (bufs->rx_buffer && bufs->rx_buffer_size / window == 0) return BLELINK_ERR_ARG;
    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg = *cfg;
    ctx->bufs = *bufs;
    ctx->window = window;
    if (bufs->rx_buffer) {
        ctx->rx_slot_size = bufs->rx_buffer_size / window;
        ctx->own_max_message = ctx->rx_slot_size;
        if (cfg->max_message && cfg->max_message < ctx->own_max_message) ctx->own_max_message = cfg->max_message;
        for (i = 0; i < window; i++) ctx->rx[i].buf = bufs->rx_buffer + (size_t)i * ctx->rx_slot_size;
    } else {
        ctx->own_max_message = cfg->max_message ? cfg->max_message : 0xFFFFFFFFu;
    }
    ctx->os_limit = BLELINK_MIN_PACKET;
    return BLELINK_OK;
}

int blelink_connect(blelink_ctx_t *ctx, uint16_t os_limit)
{
    if (!ctx) return BLELINK_ERR_ARG;
    lock(ctx);
    now_ms(ctx);
    ctx->os_limit = os_limit;
    ctx->reset_pending = 0;
    if (ctx->state != ST_CLOSED) {
        /* client-initiated reset (3): in-flight messages fail, queued ones survive */
        ctx->state = ST_OPENING;
        session_clear(ctx, BLELINK_FAIL_RESET, 0);
        ctx->reset_pending = 1;
    }
    ctx->state = ST_OPENING;
    ctx->hello_pending = 1;
    ctx->hello_tries = 0;
    ctx->hello_answers_pending = 1;
    ctx->hello_ms = ctx->now;
    ctx->tx_seq = 0;
    unlock_and_pump(ctx);
    return BLELINK_OK;
}

void blelink_set_os_limit(blelink_ctx_t *ctx, uint16_t os_limit) { ctx->os_limit = os_limit; }

void blelink_on_packet(blelink_ctx_t *ctx, const uint8_t *data, uint16_t len)
{
    blelink_packet_t pk;
    int r = blelink_decode(data, len, &pk);
    if (r == BLELINK_DECODE_EMPTY) return;
    lock(ctx);
    now_ms(ctx);
    if (pk.type == BLELINK_PKT_HELLO && r == BLELINK_DECODE_OK) {
        on_hello(ctx, &pk.hello);
        goto out;
    }
    if (ctx->state != ST_READY) goto out; /* only HELLO before READY (3) */
    if (pk.seq != ctx->rx_expected) rx_gap(ctx);
    ctx->rx_expected = (uint8_t)(pk.seq + 1u);
    if (r != BLELINK_DECODE_OK) goto out; /* short or unknown type: ignored, seq counted (2.3) */
    switch (pk.type) {
    case BLELINK_PKT_MSG_START: rx_start(ctx, &pk); break;
    case BLELINK_PKT_MSG_DATA: {
        blelink_rx_slot_t *rs = rx_find(ctx, pk.msg_id);
        if (!rs)
            rx_unknown(ctx, pk.msg_id);
        else if (rs->state == RX_RECV) /* in resync: stale post-gap data is dropped (8.1) */
            rx_accept(ctx, rs, pk.payload, pk.payload_len);
        break;
    }
    case BLELINK_PKT_MSG_RESEND: {
        blelink_rx_slot_t *rs = rx_find(ctx, pk.msg_id);
        if (!rs) {
            rx_unknown(ctx, pk.msg_id);
        } else if (pk.value != rs->received) { /* wrong offset: RESUME once, stay in resync (8.2) */
            if (!rs->mismatch_resumed) {
                rs->mismatch_resumed = 1;
                rs->state = RX_RESYNC;
                ctrl_push(ctx, BLELINK_PKT_RESUME, rs->id, 0, rs->received);
            }
        } else {
            rs->state = RX_RECV;
            rs->mismatch_resumed = 0;
            rx_accept(ctx, rs, pk.payload, pk.payload_len);
        }
        break;
    }
    case BLELINK_PKT_CANCEL: {
        blelink_rx_slot_t *rs = rx_find(ctx, pk.msg_id);
        if (rs) {
            rx_drop(ctx, rs);
            silent_add(ctx, pk.msg_id); /* packets still in flight: no RESUME for them */
        }
        break;
    }
    case BLELINK_PKT_ACK: {
        blelink_tx_slot_t *s = tx_find(ctx, pk.msg_id, 1);
        if (!s) break;
        switch (pk.code) {
        case BLELINK_ACK_OK: {
            uint16_t id = s->id;
            memset(s, 0, sizeof(*s));
            emit_simple(ctx, BLELINK_EVT_SENT, id);
            break;
        }
        case BLELINK_ACK_CRC:
            if (s->crc_retries < MAX_RESENDS) {
                s->crc_retries++;
                tx_restart(s, 0);
            } else {
                tx_fail(ctx, s, BLELINK_FAIL_CRC, 0, 0);
            }
            break;
        case BLELINK_ACK_TOO_LARGE: tx_fail(ctx, s, BLELINK_FAIL_TOO_LARGE, 0, 0); break;
        case BLELINK_ACK_BUSY: tx_fail(ctx, s, BLELINK_FAIL_BUSY, 0, 0); break;
        default: tx_fail(ctx, s, BLELINK_FAIL_REJECTED, 0, 0); break; /* 4 and reserved 5..255 */
        }
        break;
    }
    case BLELINK_PKT_RESUME: {
        blelink_tx_slot_t *s = tx_find(ctx, pk.msg_id, 1);
        uint32_t off = pk.value;
        if (!s) break; /* unknown, finished or aborted: ignore (8.2) */
        if (off > s->offset || (s->len > 0 && off >= s->len) || (s->len == 0 && off != 0)) break;
        if (s->restart_guard && off <= s->restart_offset) break; /* same gap, already restarting */
        if (++s->restarts > MAX_RESTARTS) {
            tx_fail(ctx, s, BLELINK_FAIL_LINK_UNRELIABLE, 1, BLELINK_CANCEL_GAVE_UP);
            break;
        }
        tx_restart(s, off);
        s->restart_guard = 1;
        s->restart_offset = off;
        break;
    }
    case BLELINK_PKT_PROGRESS: {
        blelink_tx_slot_t *s = tx_find(ctx, pk.msg_id, 1);
        if (s) {
            blelink_event_t e;
            memset(&e, 0, sizeof(e));
            e.type = BLELINK_EVT_PROGRESS;
            e.msg_id = s->id;
            e.total = s->len;
            e.offset = pk.value;
            emit(ctx, &e);
        }
        break;
    }
    default: break;
    }
out:
    unlock_and_pump(ctx);
}

void blelink_poll(blelink_ctx_t *ctx, uint32_t now)
{
    unsigned i;
    uint32_t t;
    lock(ctx);
    if (ctx->cfg.now_ms)
        now_ms(ctx);
    else
        ctx->now = now;
    t = blelink_timeout_ms(ctx);
    if (ctx->state == ST_OPENING && !ctx->hello_pending && elapsed(ctx->hello_ms, ctx->now, t)) {
        if (ctx->hello_tries < HELLO_RETRIES) {
            ctx->hello_tries++;
            ctx->hello_pending = 1;
            ctx->hello_answers_pending++;
            ctx->hello_ms = ctx->now;
        } else {
            ctx->state = ST_CLOSED;
            ctx->hello_answers_pending = 0;
            emit_simple(ctx, BLELINK_EVT_HANDSHAKE_FAILED, 0);
        }
    }
    if (ctx->state == ST_READY) {
        if (ctx->hello_answers_pending && elapsed(ctx->hello_ms, ctx->now, t))
            ctx->hello_answers_pending = 0; /* late HELLO answers can no longer arrive */
        for (i = 0; i < BLELINK_MAX_TX; i++) { /* ACK timeout (10) */
            blelink_tx_slot_t *s = &ctx->tx[i];
            if (s->state != TX_WAIT || !elapsed(s->last_ms, ctx->now, t)) continue;
            if (s->timeouts < MAX_RESENDS) {
                s->timeouts++;
                tx_restart(s, 0); /* whole message again, same msgId (8.3) */
            } else {
                tx_fail(ctx, s, BLELINK_FAIL_TIMEOUT, 1, BLELINK_CANCEL_GAVE_UP);
            }
        }
        for (i = 0; i < ctx->window; i++) { /* receiver idle timeout (10) */
            blelink_rx_slot_t *r = &ctx->rx[i];
            if (r->state == RX_FREE || !elapsed(r->last_ms, ctx->now, t)) continue;
            if (r->idle_resumes >= MAX_IDLE_RESUMES) {
                rx_drop(ctx, r); /* give up silently */
                continue;
            }
            r->idle_resumes++;
            r->state = RX_RESYNC;
            r->mismatch_resumed = 0;
            r->last_ms = ctx->now;
            ctrl_push(ctx, BLELINK_PKT_RESUME, r->id, 0, r->received);
        }
        for (i = 0; i < BLELINK_UNKNOWN_MEMORY; i++)
            if (ctx->unknown[i].used && elapsed(ctx->unknown[i].since_ms, ctx->now, t)) ctx->unknown[i].used = 0;
    }
    unlock_and_pump(ctx);
}

void blelink_on_ready(blelink_ctx_t *ctx)
{
    lock(ctx);
    now_ms(ctx);
    unlock_and_pump(ctx);
}

void blelink_on_disconnected(blelink_ctx_t *ctx)
{
    lock(ctx);
    now_ms(ctx);
    ctx->state = ST_CLOSED;
    ctx->hello_pending = 0;
    ctx->hello_answers_pending = 0;
    ctx->reset_pending = 0;
    session_clear(ctx, BLELINK_FAIL_DISCONNECTED, 1);
    ctx->next_msg_id = 0; /* new connection = new session (11) */
    emit_simple(ctx, BLELINK_EVT_DISCONNECTED, 0);
    unlock_and_pump(ctx);
}

int blelink_send(blelink_ctx_t *ctx, const uint8_t *data, uint32_t len, uint16_t *id)
{
    unsigned i;
    blelink_tx_slot_t *s = NULL;
    if (!ctx || (!data && len)) return BLELINK_ERR_ARG;
    if (ctx->state != ST_READY) return BLELINK_ERR_NOT_READY;
    if (len > ctx->peer.max_message) return BLELINK_ERR_TOO_LARGE; /* 0xFFFFFFFF = no limit */
    for (i = 0; i < BLELINK_MAX_TX; i++)
        if (ctx->tx[i].state == TX_FREE) {
            s = &ctx->tx[i];
            break;
        }
    if (!s) return BLELINK_ERR_FULL;
    lock(ctx);
    now_ms(ctx);
    memset(s, 0, sizeof(*s));
    s->id = ctx->next_msg_id++;
    s->data = data;
    s->len = len;
    s->crc = blelink_crc32(0, data, len);
    s->state = TX_QUEUED;
    s->mode = MODE_START;
    if (id) *id = s->id;
    unlock_and_pump(ctx);
    return BLELINK_OK;
}

int blelink_cancel(blelink_ctx_t *ctx, uint16_t id)
{
    blelink_tx_slot_t *s;
    if (!ctx) return BLELINK_ERR_ARG;
    s = tx_find(ctx, id, 0);
    if (!s) return BLELINK_ERR_UNKNOWN_ID;
    lock(ctx);
    now_ms(ctx);
    tx_fail(ctx, s, BLELINK_FAIL_CANCELLED, s->state != TX_QUEUED, BLELINK_CANCEL_APP);
    unlock_and_pump(ctx);
    return BLELINK_OK;
}
