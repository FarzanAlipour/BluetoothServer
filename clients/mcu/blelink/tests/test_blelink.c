/*
 * Host unit tests for the BleLink MCU client: codec and session against docs/protocol-test-vectors.json
 * (via the generated blelink_test_vectors.h), plus behaviour tests for the rules of docs/PROTOCOL.md.
 * No Bluetooth: a fake transport records every packet the library writes.
 */
#include <stdio.h>
#include <string.h>

#include "blelink.h"
#include "blelink_test_vectors.h"

static int g_fail, g_checks;
#define CHECK(c)                                                                                                     \
    do {                                                                                                             \
        g_checks++;                                                                                                  \
        if (!(c)) {                                                                                                  \
            g_fail++;                                                                                                \
            printf("  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);                                         \
        }                                                                                                            \
    } while (0)

/* ---- fake transport + event recorder ------------------------------------------------------------ */
typedef struct {
    uint8_t d[512];
    uint16_t n;
    int with_response;
} sent_t;

typedef struct {
    blelink_event_type_t type;
    uint16_t id;
    uint8_t reason, crc_ok, reset;
    uint32_t total, offset, len;
} rec_t;

#define MAX_SENT 2048
#define MAX_EV 256
static struct {
    sent_t sent[MAX_SENT];
    int nsent;
    int budget; /* -1 = unlimited, else packets accepted before BUSY */
    rec_t ev[MAX_EV];
    int nev;
    int nchunks; /* RX_CHUNK events are counted, not recorded (a big message would fill ev[]) */
    uint8_t msg[8192]; /* last RECEIVED, or streaming reassembly */
    uint32_t msg_len;
    int chunk_order_ok;
    int reject_start, reject_received;
    int echo; /* echo RECEIVED messages back */
    uint8_t echo_buf[8192];
    blelink_ctx_t *ctx;
} F;

static int fake_send(void *user, const uint8_t *data, uint16_t len, int with_response)
{
    (void)user;
    if (F.budget == 0) return BLELINK_TX_BUSY;
    if (F.budget > 0) F.budget--;
    if (F.nsent < MAX_SENT) {
        memcpy(F.sent[F.nsent].d, data, len);
        F.sent[F.nsent].n = len;
        F.sent[F.nsent].with_response = with_response;
    }
    F.nsent++;
    return BLELINK_TX_OK;
}

static int fake_event(void *user, const blelink_event_t *e)
{
    rec_t *r;
    (void)user;
    if (e->type == BLELINK_EVT_RX_CHUNK)
        F.nchunks++;
    else if (F.nev < MAX_EV) {
        r = &F.ev[F.nev++];
        r->type = e->type;
        r->id = e->msg_id;
        r->reason = e->reason;
        r->crc_ok = e->crc_ok;
        r->reset = e->reset;
        r->total = e->total;
        r->offset = e->offset;
        r->len = e->len;
    }
    switch (e->type) {
    case BLELINK_EVT_RECEIVED:
        memcpy(F.msg, e->data, e->len);
        F.msg_len = e->len;
        if (F.echo) {
            memcpy(F.echo_buf, e->data, e->len);
            blelink_send(F.ctx, F.echo_buf, e->len, NULL);
        }
        return F.reject_received;
    case BLELINK_EVT_RX_START: F.msg_len = 0; return F.reject_start;
    case BLELINK_EVT_RX_CHUNK:
        if (e->offset != F.msg_len) F.chunk_order_ok = 0;
        /* keep the first sizeof(F.msg) bytes; larger streamed messages are only order/CRC checked */
        if (e->offset < sizeof(F.msg)) {
            uint32_t n = e->len;
            if (n > sizeof(F.msg) - e->offset) n = (uint32_t)(sizeof(F.msg) - e->offset);
            memcpy(F.msg + e->offset, e->data, n);
        }
        F.msg_len = e->offset + e->len;
        return 0;
    case BLELINK_EVT_RX_ABORT: F.msg_len = 0; return 0;
    default: return 0;
    }
}

static int count_ev(blelink_event_type_t t)
{
    int i, c = 0;
    for (i = 0; i < F.nev; i++)
        if (F.ev[i].type == t) c++;
    return c;
}

static const rec_t *find_ev(blelink_event_type_t t)
{
    int i;
    for (i = F.nev - 1; i >= 0; i--)
        if (F.ev[i].type == t) return &F.ev[i];
    {
        /* not found: a zeroed record (type 0 is no event) so the CHECK fails instead of crashing */
        static rec_t none;
        memset(&none, 0, sizeof(none));
        return &none;
    }
}

static int count_sent(uint8_t type, int from)
{
    int i, c = 0;
    for (i = from; i < F.nsent; i++)
        if (F.sent[i].d[0] == type) c++;
    return c;
}

static const sent_t *last_sent(void) { return F.nsent ? &F.sent[F.nsent - 1] : NULL; }

static int sent_eq(int idx, const uint8_t *b, uint16_t n)
{
    return idx < F.nsent && F.sent[idx].n == n && memcmp(F.sent[idx].d, b, n) == 0;
}

/* seq-independent comparison (byte 1 is seq) */
static int sent_eq_noseq(int idx, const uint8_t *b, uint16_t n)
{
    return idx < F.nsent && F.sent[idx].n == n && F.sent[idx].d[0] == b[0] && memcmp(F.sent[idx].d + 2, b + 2, n - 2u) == 0;
}

static uint8_t pr(uint32_t i) { return (uint8_t)((i * 7u + 3u) & 0xFFu); }

static uint8_t g_payload[100000];
static void fill_payload(uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) g_payload[i] = pr(i);
}

/* ---- session helpers ----------------------------------------------------------------------------- */
static blelink_ctx_t g_ctx;
static uint8_t g_rxbuf[8192];
static uint8_t g_txpkt[512];
static uint8_t g_srv_seq;

typedef struct {
    int streaming;
    uint8_t window;
    uint32_t rx_size;
    uint16_t max_packet;
    uint32_t max_message;
    uint8_t timeout;
    uint8_t want_progress;
} setup_t;

static void reset_fake(void)
{
    memset(&F, 0, sizeof(F));
    F.budget = -1;
    F.chunk_order_ok = 1;
    F.ctx = &g_ctx;
}

static void setup(const setup_t *s)
{
    blelink_config_t cfg;
    blelink_buffers_t b;
    reset_fake();
    memset(&cfg, 0, sizeof(cfg));
    memset(&b, 0, sizeof(b));
    cfg.max_packet = s->max_packet ? s->max_packet : 244;
    cfg.max_message = s->max_message;
    cfg.window = s->window ? s->window : 1;
    cfg.timeout = s->timeout;
    cfg.want_progress = s->want_progress;
    cfg.send_packet = fake_send;
    cfg.on_event = fake_event;
    if (!s->streaming) {
        b.rx_buffer = g_rxbuf;
        b.rx_buffer_size = s->rx_size ? s->rx_size : 2048u * cfg.window;
    }
    b.tx_packet = g_txpkt;
    b.tx_packet_size = sizeof(g_txpkt);
    CHECK(blelink_init(&g_ctx, &cfg, &b) == BLELINK_OK);
}

static void feed(const uint8_t *p, uint16_t n) { blelink_on_packet(&g_ctx, p, n); }

/* server HELLO with the given limits; READY afterwards */
static void server_hello(uint16_t max_packet, uint32_t max_message, uint8_t window, uint8_t flags, uint8_t timeout)
{
    uint8_t p[12];
    blelink_hello_t h;
    h.version = 1;
    h.max_packet = max_packet;
    h.max_message = max_message;
    h.window = window;
    h.flags = flags;
    h.timeout = timeout;
    blelink_encode_hello(p, 0, &h);
    g_srv_seq = 1;
    feed(p, 12);
}

static void open_session(uint16_t os_limit, uint16_t peer_max_packet, uint8_t peer_window, uint8_t peer_flags)
{
    blelink_poll(&g_ctx, 0);
    CHECK(blelink_connect(&g_ctx, os_limit) == BLELINK_OK);
    CHECK(F.nsent == 1 && F.sent[0].d[0] == BLELINK_PKT_HELLO && F.sent[0].with_response == 1);
    server_hello(peer_max_packet, 16777216u, peer_window, peer_flags, 0);
    CHECK(blelink_is_ready(&g_ctx));
    CHECK(count_ev(BLELINK_EVT_PEER_READY) == 1);
}

static void srv_ack(uint16_t id, uint8_t status)
{
    uint8_t p[5];
    feed(p, (uint16_t)blelink_encode_ack(p, g_srv_seq++, id, status));
}
static void srv_resume(uint16_t id, uint32_t off)
{
    uint8_t p[8];
    feed(p, (uint16_t)blelink_encode_resume(p, g_srv_seq++, id, off));
}
static void srv_start(uint16_t id, uint32_t total, uint32_t crc, const uint8_t *d, uint16_t n)
{
    uint8_t p[512];
    feed(p, (uint16_t)blelink_encode_start(p, g_srv_seq++, id, total, crc, d, n));
}
static void srv_data(uint16_t id, const uint8_t *d, uint16_t n)
{
    uint8_t p[512];
    feed(p, (uint16_t)blelink_encode_data(p, g_srv_seq++, id, d, n));
}

/* server sends a whole message at packet size ps */
static void srv_message(uint16_t id, const uint8_t *d, uint32_t len, uint16_t ps)
{
    uint32_t off;
    uint16_t n = (uint16_t)(len < (uint32_t)(ps - 12) ? len : (uint32_t)(ps - 12));
    srv_start(id, len, blelink_crc32(0, d, len), d, n);
    off = n;
    while (off < len) {
        n = (uint16_t)(len - off < (uint32_t)(ps - 4) ? len - off : (uint32_t)(ps - 4));
        srv_data(id, d + off, n);
        off += n;
    }
}

/* ======================================================================================== tests == */

static void test_crc(void)
{
    int i;
    for (i = 0; i < V_CRC_IN_COUNT; i++) CHECK(blelink_crc32(0, v_crc_in[i].b, v_crc_in[i].n) == v_crc_out[i]);
    {
        const uint8_t *s = (const uint8_t *)"123456789";
        unsigned cut;
        for (cut = 0; cut <= 9; cut++) CHECK(blelink_crc32(blelink_crc32(0, s, cut), s + cut, 9 - cut) == 0xCBF43926u);
    }
    fill_payload(1000);
    CHECK(blelink_crc32(0, g_payload, 1000) == v_splits[0].crc);
}

static int hex_eq(const uint8_t *b, uint16_t n, const char *hex)
{
    uint16_t i;
    if (strlen(hex) != (size_t)n * 2u) return 0;
    for (i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1 || v != b[i]) return 0;
    }
    return 1;
}

static void test_codec(void)
{
    int i;
    for (i = 0; i < V_PKT_COUNT; i++) {
        const v_fields_t *f = &v_pkt_fields[i];
        blelink_packet_t p;
        uint8_t out[64];
        size_t n = 0;
        CHECK(blelink_decode(v_pkt[i].b, v_pkt[i].n, &p) == BLELINK_DECODE_OK);
        CHECK(p.type == f->type && p.seq == f->seq);
        switch (p.type) {
        case BLELINK_PKT_HELLO:
            CHECK(p.hello.version == f->version && p.hello.max_packet == f->max_packet &&
                  p.hello.max_message == f->max_message && p.hello.window == f->window &&
                  p.hello.flags == f->flags && p.hello.timeout == f->timeout);
            n = blelink_encode_hello(out, p.seq, &p.hello);
            break;
        case BLELINK_PKT_MSG_START:
            CHECK(p.msg_id == f->msg_id && p.value == f->value && p.crc == f->crc);
            CHECK(hex_eq(p.payload, p.payload_len, f->payload_hex));
            n = blelink_encode_start(out, p.seq, p.msg_id, p.value, p.crc, p.payload, p.payload_len);
            break;
        case BLELINK_PKT_MSG_DATA:
            CHECK(p.msg_id == f->msg_id && hex_eq(p.payload, p.payload_len, f->payload_hex));
            n = blelink_encode_data(out, p.seq, p.msg_id, p.payload, p.payload_len);
            break;
        case BLELINK_PKT_MSG_RESEND:
            CHECK(p.msg_id == f->msg_id && p.value == f->value && hex_eq(p.payload, p.payload_len, f->payload_hex));
            n = blelink_encode_resend(out, p.seq, p.msg_id, p.value, p.payload, p.payload_len);
            break;
        case BLELINK_PKT_ACK:
            CHECK(p.msg_id == f->msg_id && p.code == f->code);
            n = blelink_encode_ack(out, p.seq, p.msg_id, p.code);
            break;
        case BLELINK_PKT_CANCEL:
            CHECK(p.msg_id == f->msg_id && p.code == f->code);
            n = blelink_encode_cancel(out, p.seq, p.msg_id, p.code);
            break;
        case BLELINK_PKT_RESUME:
            CHECK(p.msg_id == f->msg_id && p.value == f->value);
            n = blelink_encode_resume(out, p.seq, p.msg_id, p.value);
            break;
        case BLELINK_PKT_PROGRESS:
            CHECK(p.msg_id == f->msg_id && p.value == f->value);
            n = blelink_encode_progress(out, p.seq, p.msg_id, p.value);
            break;
        default: CHECK(0);
        }
        CHECK(n == v_pkt[i].n && memcmp(out, v_pkt[i].b, n) == 0);
    }
    /* INFO = HELLO body */
    {
        blelink_hello_t h;
        CHECK(blelink_decode_info(v_info, V_INFO_LEN, &h) == 0);
        CHECK(h.version == 1 && h.max_packet == 509 && h.window == 4 && h.flags == 1);
        CHECK(blelink_decode_info(v_info, V_INFO_LEN - 1, &h) == BLELINK_DECODE_SHORT);
    }
    /* robustness: short, unknown, trailing bytes */
    {
        blelink_packet_t p;
        const uint8_t ack_trailing[] = {4, 1, 1, 0, 0, 0xEE, 0xFF};
        const uint8_t unknown[] = {0x42, 9, 1, 2, 3};
        const uint8_t short_resume[] = {5, 3, 7, 0, 0};
        CHECK(blelink_decode(ack_trailing, sizeof(ack_trailing), &p) == 0 && p.msg_id == 1 && p.code == 0);
        CHECK(blelink_decode(unknown, sizeof(unknown), &p) == BLELINK_DECODE_UNKNOWN && p.seq == 9);
        CHECK(blelink_decode(short_resume, sizeof(short_resume), &p) == BLELINK_DECODE_SHORT && p.seq == 3);
        CHECK(blelink_decode(unknown, 1, &p) == BLELINK_DECODE_EMPTY);
        CHECK(blelink_decode(v_pkt[0].b, 11, &p) == BLELINK_DECODE_SHORT);
    }
}

static void test_hello_vectors(void)
{
#if BLELINK_MAX_RX_WINDOW >= 4
    {
        /* vector 0 is a phone-style buffered client with 16 MiB; we run it in streaming mode (no RAM for
         * 16 MiB), which adds flags bit 1. Everything else must be byte-identical. */
        setup_t s = {1, 4, 0, 244, 16777216u, 40, 1};
        setup(&s);
        blelink_connect(&g_ctx, 244);
        CHECK(F.nsent == 1 && F.sent[0].n == v_pkt[0].n);
        CHECK(F.sent[0].d[10] == (uint8_t)(v_pkt[0].b[10] | BLELINK_HELLO_FLAG_STREAMING));
        CHECK(memcmp(F.sent[0].d, v_pkt[0].b, 10) == 0 && F.sent[0].d[11] == v_pkt[0].b[11]);
    }
#endif
    {
        setup_t s = {1, 1, 0, 20, 4096, 0, 0}; /* MCU HELLO: streaming, window 1 */
        setup(&s);
        blelink_connect(&g_ctx, 20);
        CHECK(sent_eq(0, v_pkt[1].b, v_pkt[1].n));
        CHECK(F.sent[0].with_response == 1);
    }
    {
        setup_t s = {0, 1, 4096, 20, 0, 0, 0}; /* buffered: maxMessage = buffer size, flags 0 */
        setup(&s);
        blelink_connect(&g_ctx, 20);
        CHECK(F.sent[0].d[10] == 0 && blelink_get_u32(F.sent[0].d + 5) == 4096);
    }
}

/* sender reproduces messageSplits at packetSize 20/182/244/509, peer window 1 */
static void test_split_send(void)
{
    int c, i;
    for (c = 0; c < V_SPLITS_COUNT; c++) {
        const v_split_t *v = &v_splits[c];
        setup_t s = {0, 1, 2048, 244, 0, 0, 0};
        uint16_t id = 0;
        setup(&s);
        open_session((uint16_t)v->packet_size, (uint16_t)v->packet_size, 1, 0);
        CHECK(blelink_packet_size(&g_ctx) == v->packet_size);
        fill_payload(v->len);
        g_ctx.next_msg_id = (uint16_t)v->msg_id;
        CHECK(blelink_send(&g_ctx, g_payload, v->len, &id) == BLELINK_OK && id == v->msg_id);
        CHECK(F.nsent == 1 + v->count);
        for (i = 0; i < v->count; i++) CHECK(sent_eq(1 + i, v->pk[i].b, v->pk[i].n));
        /* window 1: a second message waits for the ACK */
        CHECK(blelink_send(&g_ctx, g_payload, 10, &id) == BLELINK_OK);
        CHECK(F.nsent == 1 + v->count);
        srv_ack((uint16_t)v->msg_id, 0);
        CHECK(count_ev(BLELINK_EVT_SENT) == 1 && find_ev(BLELINK_EVT_SENT)->id == v->msg_id);
        /* the queued 10-byte message starts now (MSG_START + 1 MSG_DATA at packetSize 20, else 1 packet) */
        CHECK(F.nsent == 1 + v->count + (v->packet_size == 20 ? 2 : 1));
        CHECK(F.sent[1 + v->count].d[0] == BLELINK_PKT_MSG_START);
    }
}

/* receiver reassembles messageSplits, buffered and streaming */
static void test_split_receive(void)
{
    int c, i, streaming;
    for (streaming = 0; streaming < 2; streaming++) {
        for (c = 0; c < V_SPLITS_COUNT; c++) {
            const v_split_t *v = &v_splits[c];
            setup_t s = {streaming, 1, 1000, 512, 0, 0, 0};
            uint8_t ack[5];
            setup(&s);
            open_session(244, 244, 1, 0);
            for (i = 0; i < v->count; i++) feed(v->pk[i].b, v->pk[i].n);
            fill_payload(v->len);
            CHECK(F.msg_len == v->len && memcmp(F.msg, g_payload, v->len) == 0);
            CHECK(F.chunk_order_ok);
            if (streaming) {
                const rec_t *d = find_ev(BLELINK_EVT_RX_DONE);
                CHECK(d && d->crc_ok == 1 && d->id == v->msg_id && d->total == v->len);
                CHECK(count_ev(BLELINK_EVT_RX_START) == 1);
            } else {
                CHECK(count_ev(BLELINK_EVT_RECEIVED) == 1 && find_ev(BLELINK_EVT_RECEIVED)->id == v->msg_id);
            }
            blelink_encode_ack(ack, 1, (uint16_t)v->msg_id, 0);
            CHECK(F.nsent == 2 && sent_eq(1, ack, 5));
            CHECK(g_ctx.gaps == 0);
        }
    }
}

static void test_boundaries(void)
{
    int c, i, streaming;
    for (c = 0; c < V_BOUNDS_COUNT; c++) {
        const v_bound_t *v = &v_bounds[c];
        setup_t s = {0, 1, 2048, 20, 0, 0, 0};
        setup(&s);
        open_session(20, 20, 1, 0);
        fill_payload(v->len);
        g_ctx.next_msg_id = 5;
        CHECK(blelink_send(&g_ctx, g_payload, v->len, NULL) == BLELINK_OK);
        CHECK(F.nsent == 1 + v->count);
        for (i = 0; i < v->count; i++) CHECK(sent_eq(1 + i, v->pk[i].b, v->pk[i].n));
        for (streaming = 0; streaming < 2; streaming++) {
            setup_t r = {streaming, 1, 2048, 20, 0, 0, 0};
            setup(&r);
            open_session(20, 20, 1, 0);
            for (i = 0; i < v->count; i++) feed(v->pk[i].b, v->pk[i].n);
            CHECK(F.msg_len == v->len && memcmp(F.msg, g_payload, v->len) == 0);
            CHECK(last_sent()->d[0] == BLELINK_PKT_ACK && last_sent()->d[4] == 0);
        }
    }
}

static void test_interleaving(void)
{
#if BLELINK_MAX_RX_WINDOW >= 2
    {
        setup_t s = {0, 2, 4096, 20, 0, 0, 0};
        int i;
        setup(&s);
        open_session(20, 20, 2, 0);
        for (i = 0; i < V_IL_PK_COUNT; i++) feed(v_il_pk[i].b, v_il_pk[i].n);
        CHECK(count_ev(BLELINK_EVT_RECEIVED) == 2);
        {
            int k = 0;
            for (i = 0; i < F.nev; i++)
                if (F.ev[i].type == BLELINK_EVT_RECEIVED) {
                    CHECK(F.ev[i].id == v_il_order[k]);
                    k++;
                }
        }
        CHECK(F.nsent == 3 && sent_eq(1, v_il_ack[0].b, v_il_ack[0].n) && sent_eq(2, v_il_ack[1].b, v_il_ack[1].n));
    }
#endif
#if BLELINK_MAX_TX >= 2
    {
        setup_t s = {0, 1, 2048, 20, 0, 0, 0};
        static const uint8_t ping[] = {'p', 'i', 'n', 'g'};
        int i;
        setup(&s);
        open_session(20, 20, 2, 0);
        fill_payload(V_IL_LEN10);
        CHECK(blelink_crc32(0, g_payload, V_IL_LEN10) == V_IL_CRC10 && blelink_crc32(0, ping, 4) == V_IL_CRC11);
        g_ctx.next_msg_id = 10;
        F.budget = 2; /* the stack takes 2 packets, then reports busy */
        CHECK(blelink_send(&g_ctx, g_payload, V_IL_LEN10, NULL) == BLELINK_OK);
        CHECK(blelink_send(&g_ctx, ping, 4, NULL) == BLELINK_OK);
        CHECK(F.nsent == 3);
        F.budget = -1;
        blelink_on_ready(&g_ctx);
        CHECK(F.nsent == 1 + V_IL_PK_COUNT);
        for (i = 0; i < V_IL_PK_COUNT; i++) CHECK(sent_eq(1 + i, v_il_pk[i].b, v_il_pk[i].n));
        srv_ack(11, 0);
        srv_ack(10, 0);
        CHECK(count_ev(BLELINK_EVT_SENT) == 2);
    }
#endif
}

/* resume vector: receiver side. fix_seq = 1 renumbers the resend packets to seq 6.. (NOTES.md, spec issue S1) */
static void resume_receive(int streaming, int fix_seq)
{
    setup_t s = {streaming, 1, 2048, 20, 0, 0, 0};
    int i, before;
    setup(&s);
    open_session(20, 20, 1, 0);
    for (i = 0; i < V_RS_ORIG_COUNT; i++) {
        if (i == V_RS_LOST) continue;
        feed(v_rs_orig[i].b, v_rs_orig[i].n);
        if (i == V_RS_LOST + 1) CHECK(F.nsent == 2 && sent_eq(1, v_rs_resume, V_RS_RESUME_LEN));
    }
    CHECK(F.nsent == 2); /* seq 5 was dropped silently (resync) */
    before = F.nsent;
    for (i = 0; i < V_RS_RESEND_COUNT; i++) {
        uint8_t p[32];
        memcpy(p, v_rs_resend[i].b, v_rs_resend[i].n);
        if (fix_seq) p[1] = (uint8_t)(6 + i);
        feed(p, v_rs_resend[i].n);
    }
    fill_payload(V_RS_LEN);
    CHECK(F.msg_len == V_RS_LEN && memcmp(F.msg, g_payload, V_RS_LEN) == 0);
    CHECK(F.chunk_order_ok);
    CHECK(last_sent()->d[0] == BLELINK_PKT_ACK && last_sent()->d[4] == 0);
    /* with the vector as published the resend starts at seq 7 (expected 6): one more gap, one more RESUME */
    CHECK(count_sent(BLELINK_PKT_RESUME, before) == (fix_seq ? 0 : 1));
}

static void test_resume(void)
{
    resume_receive(0, 1);
    resume_receive(1, 1);
    resume_receive(0, 0);
    /* sender side: RESUME(20, 24) after all 5 packets -> MSG_RESEND(24) + MSG_DATA (seq 6, 7, 8) */
    {
        setup_t s = {0, 1, 2048, 20, 0, 0, 0};
        int i;
        setup(&s);
        open_session(20, 20, 1, 0);
        fill_payload(V_RS_LEN);
        g_ctx.next_msg_id = 20;
        blelink_send(&g_ctx, g_payload, V_RS_LEN, NULL);
        CHECK(F.nsent == 1 + V_RS_ORIG_COUNT);
        for (i = 0; i < V_RS_ORIG_COUNT; i++) CHECK(sent_eq(1 + i, v_rs_orig[i].b, v_rs_orig[i].n));
        feed(v_rs_resume, V_RS_RESUME_LEN);
        CHECK(F.nsent == 1 + V_RS_ORIG_COUNT + V_RS_RESEND_COUNT);
        for (i = 0; i < V_RS_RESEND_COUNT; i++) {
            CHECK(sent_eq_noseq(1 + V_RS_ORIG_COUNT + i, v_rs_resend[i].b, v_rs_resend[i].n));
            CHECK(F.sent[1 + V_RS_ORIG_COUNT + i].d[1] == 6 + i);
        }
    }
}

static void test_ack_status(void)
{
    setup_t s = {0, 1, 2048, 20, 0, 0, 0};
    static const uint8_t m[] = "hello";
    uint16_t id;
    int i, n0;
    setup(&s);
    open_session(20, 20, 4, 0);
    /* CRC: 3 automatic resends with the same id, then FAILED crc */
    blelink_send(&g_ctx, m, 5, &id);
    for (i = 0; i < 3; i++) {
        n0 = F.nsent;
        srv_ack(id, 1);
        CHECK(F.nsent == n0 + 1 && last_sent()->d[0] == BLELINK_PKT_MSG_START && blelink_get_u16(last_sent()->d + 2) == id);
    }
    srv_ack(id, 1);
    CHECK(find_ev(BLELINK_EVT_FAILED) && find_ev(BLELINK_EVT_FAILED)->reason == BLELINK_FAIL_CRC);
    /* too large / busy / rejected / reserved */
    {
        const uint8_t st[] = {2, 3, 4, 9};
        const uint8_t want[] = {BLELINK_FAIL_TOO_LARGE, BLELINK_FAIL_BUSY, BLELINK_FAIL_REJECTED, BLELINK_FAIL_REJECTED};
        for (i = 0; i < 4; i++) {
            blelink_send(&g_ctx, m, 5, &id);
            srv_ack(id, st[i]);
            CHECK(find_ev(BLELINK_EVT_FAILED)->id == id && find_ev(BLELINK_EVT_FAILED)->reason == want[i]);
        }
    }
    /* early ACK while still sending stops the message */
    fill_payload(200);
    F.budget = 2;
    blelink_send(&g_ctx, g_payload, 200, &id);
    srv_ack(id, 4);
    n0 = F.nsent;
    F.budget = -1;
    blelink_on_ready(&g_ctx);
    CHECK(F.nsent == n0);
    CHECK(find_ev(BLELINK_EVT_FAILED)->id == id);
    /* ACK for an unknown id is ignored */
    n0 = F.nev;
    srv_ack(999, 0);
    CHECK(F.nev == n0);
    /* local limits */
    CHECK(blelink_send(&g_ctx, NULL, 5, &id) == BLELINK_ERR_ARG);
}

static void test_ack_timeout(void)
{
    setup_t s = {0, 1, 2048, 20, 0, 0, 0};
    static const uint8_t m[] = "hello";
    uint16_t id;
    int i, n0;
    setup(&s);
    open_session(20, 20, 1, 0);
    CHECK(blelink_timeout_ms(&g_ctx) == 10000);
    blelink_send(&g_ctx, m, 5, &id);
    for (i = 1; i <= 3; i++) {
        n0 = F.nsent;
        blelink_poll(&g_ctx, (uint32_t)i * 10000u - 1u);
        CHECK(F.nsent == n0);
        blelink_poll(&g_ctx, (uint32_t)i * 10000u);
        CHECK(F.nsent == n0 + 1 && last_sent()->d[0] == BLELINK_PKT_MSG_START && blelink_get_u16(last_sent()->d + 2) == id);
    }
    blelink_poll(&g_ctx, 40000u);
    CHECK(find_ev(BLELINK_EVT_FAILED) && find_ev(BLELINK_EVT_FAILED)->reason == BLELINK_FAIL_TIMEOUT);
    CHECK(last_sent()->d[0] == BLELINK_PKT_CANCEL && last_sent()->d[4] == BLELINK_CANCEL_GAVE_UP);
    /* lost ACK + resend: the server answers with its remembered ACK */
    blelink_send(&g_ctx, m, 5, &id);
    blelink_poll(&g_ctx, 50000u);
    srv_ack(id, 0);
    CHECK(find_ev(BLELINK_EVT_SENT)->id == id);
    /* larger timeout from the server HELLO wins */
    setup(&s);
    blelink_connect(&g_ctx, 20);
    server_hello(20, 1000, 1, 0, 120);
    CHECK(blelink_timeout_ms(&g_ctx) == 30000);
}

static void test_hello_timeout(void)
{
    setup_t s = {0, 1, 2048, 20, 0, 0, 0};
    int i;
    setup(&s);
    blelink_poll(&g_ctx, 1000);
    blelink_connect(&g_ctx, 244);
    CHECK(blelink_send(&g_ctx, (const uint8_t *)"x", 1, NULL) == BLELINK_ERR_NOT_READY);
    CHECK(blelink_packet_size(&g_ctx) == 20);
    for (i = 1; i <= 3; i++) {
        blelink_poll(&g_ctx, 1000u + (uint32_t)i * 10000u);
        CHECK(F.nsent == 1 + i && last_sent()->d[0] == BLELINK_PKT_HELLO && last_sent()->d[1] == 0);
    }
    blelink_poll(&g_ctx, 1000u + 4u * 10000u);
    CHECK(count_ev(BLELINK_EVT_HANDSHAKE_FAILED) == 1 && !blelink_is_ready(&g_ctx));
    /* packets before READY are ignored */
    setup(&s);
    blelink_connect(&g_ctx, 244);
    srv_ack(0, 0);
    srv_start(0, 1, blelink_crc32(0, (const uint8_t *)"a", 1), (const uint8_t *)"a", 1);
    CHECK(F.nsent == 1 && F.nev == 0);
}

/* A HELLO re-sent after a timeout gets two answers; the second must not start a HELLO ping-pong. */
static void test_hello_late_answer(void)
{
    setup_t s = {0, 1, 2048, 20, 0, 0, 0};
    setup(&s);
    blelink_poll(&g_ctx, 0);
    blelink_connect(&g_ctx, 244);
    blelink_poll(&g_ctx, 10000);
    CHECK(F.nsent == 2);
    server_hello(244, 100000, 1, 0, 0);
    CHECK(blelink_is_ready(&g_ctx));
    server_hello(244, 100000, 1, 0, 0);
    CHECK(F.nsent == 2); /* no HELLO answer */
    CHECK(count_ev(BLELINK_EVT_PEER_READY) == 2 && find_ev(BLELINK_EVT_PEER_READY)->reset == 1);
    /* a later server-initiated reset is answered again */
    server_hello(244, 100000, 1, 0, 0);
    CHECK(F.nsent == 3 && last_sent()->d[0] == BLELINK_PKT_HELLO);
}

static void test_peer_reset(void)
{
    setup_t s = {1, 1, 0, 244, 0, 0, 0};
    uint16_t id;
    int n0;
    setup(&s);
    open_session(244, 244, 1, 0);
    fill_payload(500);
    blelink_send(&g_ctx, g_payload, 500, &id);
    srv_start(7, 100, 0, g_payload, 10); /* partial receive */
    n0 = F.nsent;
    server_hello(244, 1000, 1, 0, 0);
    CHECK(find_ev(BLELINK_EVT_FAILED)->id == id && find_ev(BLELINK_EVT_FAILED)->reason == BLELINK_FAIL_RESET);
    CHECK(count_ev(BLELINK_EVT_RX_ABORT) == 1);
    CHECK(F.nsent == n0 + 1 && last_sent()->d[0] == BLELINK_PKT_HELLO && last_sent()->d[1] == 0);
    CHECK(find_ev(BLELINK_EVT_PEER_READY)->reset == 1 && blelink_is_ready(&g_ctx));
    /* seq restarted: next packet seq 1 */
    blelink_send(&g_ctx, g_payload, 5, &id);
    CHECK(last_sent()->d[1] == 1);
    /* client-initiated reset: in-flight fails RESET, HELLO goes out, READY after server answer */
    blelink_connect(&g_ctx, 244);
    CHECK(find_ev(BLELINK_EVT_FAILED)->id == id && find_ev(BLELINK_EVT_FAILED)->reason == BLELINK_FAIL_RESET);
    CHECK(!blelink_is_ready(&g_ctx) && last_sent()->d[0] == BLELINK_PKT_HELLO);
    server_hello(244, 1000, 1, 0, 0);
    CHECK(blelink_is_ready(&g_ctx) && find_ev(BLELINK_EVT_PEER_READY)->reset == 1);
}

static void test_disconnect(void)
{
    setup_t s = {0, 1, 2048, 20, 0, 0, 0};
    uint16_t a, b;
    setup(&s);
    open_session(20, 20, 1, 0);
    blelink_send(&g_ctx, (const uint8_t *)"abc", 3, &a);
    blelink_send(&g_ctx, (const uint8_t *)"def", 3, &b); /* queued behind window 1 */
    blelink_on_disconnected(&g_ctx);
    CHECK(count_ev(BLELINK_EVT_FAILED) == 2 && find_ev(BLELINK_EVT_FAILED)->reason == BLELINK_FAIL_DISCONNECTED);
    CHECK(count_ev(BLELINK_EVT_DISCONNECTED) == 1);
    CHECK(blelink_send(&g_ctx, (const uint8_t *)"x", 1, NULL) == BLELINK_ERR_NOT_READY);
    /* reconnect: msgIds and seq restart */
    reset_fake();
    open_session(20, 20, 1, 0);
    blelink_send(&g_ctx, (const uint8_t *)"x", 1, &a);
    CHECK(a == 0 && last_sent()->d[1] == 1);
}

static void test_flow_control(void)
{
    setup_t s = {0, 1, 2048, 20, 0, 0, 0};
    int i;
    setup(&s);
    open_session(20, 20, 1, 0);
    fill_payload(1000);
    g_ctx.next_msg_id = 258;
    F.budget = 0;
    blelink_send(&g_ctx, g_payload, 1000, NULL);
    CHECK(F.nsent == 1);
    while (F.nsent < 1 + v_splits[0].count) { /* the stack takes 3 packets per "connection event" */
        F.budget = 3;
        blelink_on_ready(&g_ctx);
    }
    for (i = 0; i < v_splits[0].count; i++) CHECK(sent_eq(1 + i, v_splits[0].pk[i].b, v_splits[0].pk[i].n));
    /* pending control packets go out first once the stack is ready */
    F.budget = 0;
    srv_message(3, (const uint8_t *)"abc", 3, 20);
    CHECK(F.nsent == 1 + v_splits[0].count);
    F.budget = -1;
    blelink_poll(&g_ctx, 1);
    CHECK(last_sent()->d[0] == BLELINK_PKT_ACK);
}

static void test_receive_rules(void)
{
    setup_t s = {0, 1, 100, 20, 0, 0, 0};
    uint8_t p[32];
    int n0;
    setup(&s);
    open_session(20, 20, 1, 0);
    /* duplicate after lost ACK: re-ACK, no second delivery, its packets ignored silently */
    srv_message(1, (const uint8_t *)"hello world, hello!", 19, 20);
    CHECK(count_ev(BLELINK_EVT_RECEIVED) == 1 && last_sent()->d[0] == BLELINK_PKT_ACK);
    n0 = F.nsent;
    srv_message(1, (const uint8_t *)"hello world, hello!", 19, 20);
    CHECK(count_ev(BLELINK_EVT_RECEIVED) == 1);
    CHECK(F.nsent == n0 + 1 && last_sent()->d[0] == BLELINK_PKT_ACK && blelink_get_u16(last_sent()->d + 2) == 1);
    /* unknown id MSG_DATA: RESUME(id, 0) once */
    n0 = F.nsent;
    srv_data(40, (const uint8_t *)"zz", 2);
    srv_data(40, (const uint8_t *)"zz", 2);
    CHECK(F.nsent == n0 + 1 && last_sent()->d[0] == BLELINK_PKT_RESUME && blelink_get_u16(last_sent()->d + 2) == 40 &&
          blelink_get_u32(last_sent()->d + 4) == 0);
    /* too large: early ACK 2, data ignored silently */
    n0 = F.nsent;
    srv_start(41, 101, 0, (const uint8_t *)"a", 1);
    srv_data(41, (const uint8_t *)"zz", 2);
    CHECK(F.nsent == n0 + 1 && last_sent()->d[4] == BLELINK_ACK_TOO_LARGE);
    /* window exceeded: ACK 3 */
    srv_start(42, 50, 0, (const uint8_t *)"a", 1);
    n0 = F.nsent;
    srv_start(43, 5, 0, (const uint8_t *)"a", 1);
    CHECK(F.nsent == n0 + 1 && blelink_get_u16(last_sent()->d + 2) == 43 && last_sent()->d[4] == BLELINK_ACK_BUSY);
    /* MSG_START for the id being received restarts it */
    {
        static const uint8_t m[] = "0123456789";
        srv_start(42, 10, blelink_crc32(0, m, 10), m, 8);
        srv_data(42, m + 8, 2);
        CHECK(F.msg_len == 10 && memcmp(F.msg, m, 10) == 0 && last_sent()->d[4] == 0);
    }
    /* MSG_RESEND with the wrong offset: RESUME(id, received) once */
    {
        static const uint8_t m[] = "abcdefghijklmnopqrstuvwxyz";
        srv_start(44, 26, blelink_crc32(0, m, 26), m, 8);
        n0 = F.nsent;
        feed(p, (uint16_t)blelink_encode_resend(p, g_srv_seq++, 44, 12, m + 12, 4));
        feed(p, (uint16_t)blelink_encode_resend(p, g_srv_seq++, 44, 12, m + 12, 4));
        CHECK(F.nsent == n0 + 1 && last_sent()->d[0] == BLELINK_PKT_RESUME && blelink_get_u32(last_sent()->d + 4) == 8);
        srv_data(44, m + 8, 4); /* in resync: dropped */
        feed(p, (uint16_t)blelink_encode_resend(p, g_srv_seq++, 44, 8, m + 8, 12));
        srv_data(44, m + 20, 6);
        CHECK(F.msg_len == 26 && memcmp(F.msg, m, 26) == 0 && last_sent()->d[4] == 0);
    }
    /* CRC mismatch: ACK 1, not remembered: the retry with the same id is accepted */
    {
        static const uint8_t m[] = "crc!";
        int got = count_ev(BLELINK_EVT_RECEIVED);
        srv_start(45, 4, 0x12345678u, m, 4);
        CHECK(last_sent()->d[4] == BLELINK_ACK_CRC && count_ev(BLELINK_EVT_RECEIVED) == got);
        srv_start(45, 4, blelink_crc32(0, m, 4), m, 4);
        CHECK(last_sent()->d[4] == 0 && count_ev(BLELINK_EVT_RECEIVED) == got + 1);
    }
    /* more data than totalLen: ACK 1 */
    srv_start(46, 2, 0, (const uint8_t *)"abc", 3);
    CHECK(last_sent()->d[4] == BLELINK_ACK_CRC);
    /* application rejects: ACK 4 */
    F.reject_received = 1;
    srv_message(47, (const uint8_t *)"no", 2, 20);
    CHECK(last_sent()->d[4] == BLELINK_ACK_REJECTED);
    F.reject_received = 0;
    /* empty message */
    srv_start(48, 0, 0, NULL, 0);
    CHECK(find_ev(BLELINK_EVT_RECEIVED)->id == 48 && find_ev(BLELINK_EVT_RECEIVED)->len == 0 && last_sent()->d[4] == 0);
    /* unknown packet type and short packets: ignored, seq counted (no gap) */
    {
        const uint8_t unk[] = {0x55, 0, 1, 2};
        const uint8_t shrt[] = {BLELINK_PKT_ACK, 0, 1};
        uint32_t gaps = g_ctx.gaps;
        uint8_t u[4], sh[3];
        memcpy(u, unk, 4);
        u[1] = g_srv_seq++;
        memcpy(sh, shrt, 3);
        sh[1] = g_srv_seq++;
        n0 = F.nsent;
        feed(u, 4);
        feed(sh, 3);
        srv_ack(999, 0);
        CHECK(g_ctx.gaps == gaps && F.nsent == n0);
    }
}

static void test_streaming(void)
{
    setup_t s = {1, 1, 0, 244, 1000000u, 0, 0};
    uint16_t i;
    setup(&s);
    open_session(244, 244, 1, BLELINK_HELLO_FLAG_PROGRESS);
    CHECK(F.sent[0].d[10] == BLELINK_HELLO_FLAG_STREAMING && blelink_get_u32(F.sent[0].d + 5) == 1000000u);
    /* message larger than any RAM buffer, with PROGRESS (server asked for it) */
    fill_payload(70000);
    srv_message(9, g_payload, 70000, 244);
    CHECK(count_sent(BLELINK_PKT_PROGRESS, 0) == 1);
    for (i = 0; i < F.nsent; i++)
        if (F.sent[i].d[0] == BLELINK_PKT_PROGRESS) {
            uint32_t b = blelink_get_u32(F.sent[i].d + 4);
            CHECK(b >= 65536u && b < 65536u + 240u);
        }
    CHECK(find_ev(BLELINK_EVT_RX_DONE)->crc_ok == 1 && last_sent()->d[0] == BLELINK_PKT_ACK && last_sent()->d[4] == 0);
    CHECK(F.chunk_order_ok && memcmp(F.msg, g_payload, 8192) == 0);
    /* CRC failure is reported in RX_DONE and answered with ACK 1 */
    srv_start(10, 3, 1, (const uint8_t *)"abc", 3);
    CHECK(find_ev(BLELINK_EVT_RX_DONE)->crc_ok == 0 && last_sent()->d[4] == BLELINK_ACK_CRC);
    /* the application refuses at RX_START: ACK 4 right away */
    F.reject_start = 1;
    srv_start(11, 300, 0, g_payload, 10);
    CHECK(last_sent()->d[4] == BLELINK_ACK_REJECTED);
    F.reject_start = 0;
    /* CANCEL from the server: RX_ABORT, no ACK, later packets silent */
    {
        uint8_t p[8];
        int n0;
        srv_start(12, 300, 0, g_payload, 10);
        n0 = F.nsent;
        feed(p, (uint16_t)blelink_encode_cancel(p, g_srv_seq++, 12, 0));
        srv_data(12, g_payload, 10);
        CHECK(find_ev(BLELINK_EVT_RX_ABORT)->id == 12 && F.nsent == n0);
    }
    /* maxMessage enforced */
    srv_start(13, 1000001u, 0, g_payload, 10);
    CHECK(last_sent()->d[4] == BLELINK_ACK_TOO_LARGE);
    /* seq wraps 255 -> 0 without a false gap: 6000 bytes at packet size 20 = 375 packets */
    {
        setup_t w = {1, 1, 0, 20, 0, 0, 0};
        setup(&w);
        open_session(20, 20, 1, 0);
        fill_payload(6000);
        srv_message(14, g_payload, 6000, 20);
        CHECK(g_ctx.gaps == 0 && find_ev(BLELINK_EVT_RX_DONE)->crc_ok == 1);
    }
}

static void test_idle_timeout(void)
{
    setup_t s = {0, 1, 2048, 20, 0, 0, 0};
    int i, n0;
    setup(&s);
    open_session(20, 20, 1, 0);
    srv_start(5, 100, 0, g_payload, 8);
    for (i = 1; i <= 3; i++) {
        n0 = F.nsent;
        blelink_poll(&g_ctx, (uint32_t)i * 10000u);
        CHECK(F.nsent == n0 + 1 && last_sent()->d[0] == BLELINK_PKT_RESUME && blelink_get_u32(last_sent()->d + 4) == 8);
    }
    n0 = F.nsent;
    blelink_poll(&g_ctx, 40000u); /* gives up silently */
    CHECK(F.nsent == n0);
    srv_data(5, g_payload, 4); /* now unknown -> RESUME(5, 0) */
    CHECK(last_sent()->d[0] == BLELINK_PKT_RESUME && blelink_get_u32(last_sent()->d + 4) == 0);
}

static void test_resume_sender_rules(void)
{
    setup_t s = {0, 1, 2048, 20, 0, 0, 0};
    uint16_t id;
    int i, n0;
    setup(&s);
    open_session(20, 20, 1, 0);
    fill_payload(100);
    blelink_send(&g_ctx, g_payload, 100, &id);
    /* RESUME offsets that cannot be valid are ignored */
    n0 = F.nsent;
    srv_resume(id, 100);
    srv_resume(id, 101);
    srv_resume(99, 0);
    CHECK(F.nsent == n0);
    /* back-to-back RESUMEs while the restart is still pending: only the first counts */
    F.budget = 0;
    srv_resume(id, 24);
    srv_resume(id, 24);
    srv_resume(id, 8);
    F.budget = -1;
    blelink_on_ready(&g_ctx);
    CHECK(F.sent[n0].d[0] == BLELINK_PKT_MSG_RESEND && blelink_get_u32(F.sent[n0].d + 4) == 24);
    CHECK(count_sent(BLELINK_PKT_MSG_RESEND, n0) == 1);
    /* RESUME(id, 0) = full restart with MSG_START */
    n0 = F.nsent;
    srv_resume(id, 0);
    CHECK(F.sent[n0].d[0] == BLELINK_PKT_MSG_START && blelink_get_u16(F.sent[n0].d + 2) == id);
    /* more than 8 restarts: link_unreliable + CANCEL(1). Two restarts so far (24, then 0): six more
     * are allowed (8 in total), the ninth fails the message (8.2). */
    for (i = 0; i < 6; i++) srv_resume(id, 24);
    CHECK(count_ev(BLELINK_EVT_FAILED) == 0);
    srv_resume(id, 24);
    CHECK(find_ev(BLELINK_EVT_FAILED)->reason == BLELINK_FAIL_LINK_UNRELIABLE);
    CHECK(last_sent()->d[0] == BLELINK_PKT_CANCEL && last_sent()->d[4] == BLELINK_CANCEL_GAVE_UP);
    /* app cancel */
    F.budget = 1;
    blelink_send(&g_ctx, g_payload, 100, &id);
    F.budget = -1;
    CHECK(blelink_cancel(&g_ctx, id) == BLELINK_OK);
    CHECK(find_ev(BLELINK_EVT_FAILED)->reason == BLELINK_FAIL_CANCELLED && last_sent()->d[0] == BLELINK_PKT_CANCEL &&
          last_sent()->d[4] == BLELINK_CANCEL_APP);
    CHECK(blelink_cancel(&g_ctx, id) == BLELINK_ERR_UNKNOWN_ID);
    /* server PROGRESS is reported */
    blelink_send(&g_ctx, g_payload, 100, &id);
    {
        uint8_t p[8];
        feed(p, (uint16_t)blelink_encode_progress(p, g_srv_seq++, id, 50));
        CHECK(find_ev(BLELINK_EVT_PROGRESS)->id == id && find_ev(BLELINK_EVT_PROGRESS)->offset == 50);
    }
}

static void test_limits_and_echo(void)
{
    setup_t s = {0, 1, 2048, 20, 0, 0, 0};
    uint16_t id;
    setup(&s);
    blelink_connect(&g_ctx, 244);
    server_hello(244, 50, 1, 0, 0); /* server accepts 50-byte messages */
    CHECK(blelink_send(&g_ctx, g_payload, 51, &id) == BLELINK_ERR_TOO_LARGE);
    /* packetSize = min(server maxPacket, os limit), clamped */
    CHECK(blelink_packet_size(&g_ctx) == 244);
    blelink_set_os_limit(&g_ctx, 100);
    CHECK(blelink_packet_size(&g_ctx) == 100);
    blelink_set_os_limit(&g_ctx, 5);
    CHECK(blelink_packet_size(&g_ctx) == 20);
    blelink_set_os_limit(&g_ctx, 244);
    server_hello(1000, 50, 1, 0, 0); /* maxPacket above 512 is treated as 512 (4.1) */
    CHECK(blelink_packet_size(&g_ctx) == 244);
    blelink_set_os_limit(&g_ctx, 600);
    CHECK(blelink_packet_size(&g_ctx) == 512);
    blelink_set_os_limit(&g_ctx, 244);
    /* echo from inside the event callback (re-entrancy) */
    F.echo = 1;
    blelink_set_os_limit(&g_ctx, 20);
    srv_message(3, (const uint8_t *)"echo me please, twice", 21, 20);
    F.echo = 0;
    CHECK(count_sent(BLELINK_PKT_ACK, 0) == 1);
    CHECK(last_sent()->d[0] == BLELINK_PKT_MSG_DATA || last_sent()->d[0] == BLELINK_PKT_MSG_START);
    {
        /* reassemble what we sent back and compare */
        int i, start = -1;
        uint8_t buf[64];
        uint32_t got = 0;
        for (i = 0; i < F.nsent; i++)
            if (F.sent[i].d[0] == BLELINK_PKT_MSG_START) start = i;
        CHECK(start >= 0);
        if (start >= 0) {
            memcpy(buf, F.sent[start].d + 12, F.sent[start].n - 12u);
            got = F.sent[start].n - 12u;
            for (i = start + 1; i < F.nsent; i++)
                if (F.sent[i].d[0] == BLELINK_PKT_MSG_DATA) {
                    memcpy(buf + got, F.sent[i].d + 4, F.sent[i].n - 4u);
                    got += F.sent[i].n - 4u;
                }
            CHECK(got == 21 && memcmp(buf, "echo me please, twice", 21) == 0);
        }
    }
    /* bad configs */
    {
        blelink_config_t cfg;
        blelink_buffers_t b;
        memset(&cfg, 0, sizeof(cfg));
        memset(&b, 0, sizeof(b));
        CHECK(blelink_init(&g_ctx, &cfg, &b) == BLELINK_ERR_ARG);
        cfg.send_packet = fake_send;
        b.tx_packet = g_txpkt;
        b.tx_packet_size = 19;
        CHECK(blelink_init(&g_ctx, &cfg, &b) == BLELINK_ERR_ARG);
        b.tx_packet_size = 20;
        cfg.window = BLELINK_MAX_RX_WINDOW + 1 > 255 ? 255 : BLELINK_MAX_RX_WINDOW + 1;
        if (BLELINK_MAX_RX_WINDOW < 255) CHECK(blelink_init(&g_ctx, &cfg, &b) == BLELINK_ERR_ARG);
        cfg.window = 1;
        CHECK(blelink_init(&g_ctx, &cfg, &b) == BLELINK_OK);
        /* tx buffer smaller than the negotiated packet size caps the packet size */
        blelink_connect(&g_ctx, 244);
        server_hello(244, 1000, 1, 0, 0);
        CHECK(blelink_packet_size(&g_ctx) == 20);
    }
}

int main(void)
{
    struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        {"crc", test_crc},
        {"codec", test_codec},
        {"hello_vectors", test_hello_vectors},
        {"split_send", test_split_send},
        {"split_receive", test_split_receive},
        {"boundaries", test_boundaries},
        {"interleaving", test_interleaving},
        {"resume", test_resume},
        {"ack_status", test_ack_status},
        {"ack_timeout", test_ack_timeout},
        {"hello_timeout", test_hello_timeout},
        {"hello_late_answer", test_hello_late_answer},
        {"peer_reset", test_peer_reset},
        {"disconnect", test_disconnect},
        {"flow_control", test_flow_control},
        {"receive_rules", test_receive_rules},
        {"streaming", test_streaming},
        {"idle_timeout", test_idle_timeout},
        {"resume_sender_rules", test_resume_sender_rules},
        {"limits_and_echo", test_limits_and_echo},
    };
    size_t i;
    setvbuf(stdout, NULL, _IONBF, 0); /* see progress even if a test hangs */
    printf("blelink tests: RX_WINDOW=%d TX=%d CRC32_IMPL=%d\n", BLELINK_MAX_RX_WINDOW, BLELINK_MAX_TX,
           BLELINK_CRC32_IMPL);
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_fail;
        tests[i].fn();
        printf("%-22s %s\n", tests[i].name, g_fail == before ? "ok" : "FAIL");
    }
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
