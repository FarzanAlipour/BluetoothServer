/*
 * Host test for the shared example application (examples/common/demo_app.c): the code the ESP32 and
 * Zephyr ports run, driven through a fake transport and a scripted server. Covers what the firmware
 * does after the BLE part is up: HELLO, greeting, echo, echo-loop guard, flow control, reconnect.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "blelink.h"
#include "demo_transport.h"

#define GREETING "hello from mcu" /* demo_app.c with the default DEMO_BOARD_NAME */

static int g_fail, g_checks;
#define CHECK(c)                                                                                                     \
    do {                                                                                                             \
        g_checks++;                                                                                                  \
        if (!(c)) {                                                                                                  \
            g_fail++;                                                                                                \
            printf("  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);                                         \
        }                                                                                                            \
    } while (0)

/* ---- fake transport ---------------------------------------------------------------------------- */
#define QMAX 64
static demo_evt_t q[QMAX];
static int q_head, q_count;
static uint32_t now;
static int budget = -1, disconnects;

typedef struct {
    uint8_t d[DEMO_MAX_PACKET];
    uint16_t n;
    int rsp;
} pkt_t;
static pkt_t sent[256];
static int nsent;
static uint8_t srv_seq;

static void push(const demo_evt_t *e)
{
    if (q_count < QMAX) q[(q_head + q_count++) % QMAX] = *e;
}

int demo_transport_start(void) { return 0; }
int demo_transport_wait(demo_evt_t *e, uint32_t timeout_ms)
{
    if (!q_count) {
        now += timeout_ms;
        return 0;
    }
    *e = q[q_head];
    q_head = (q_head + 1) % QMAX;
    q_count--;
    return 1;
}
int demo_transport_send(void *user, const uint8_t *data, uint16_t len, int with_response)
{
    (void)user;
    if (budget == 0) return BLELINK_TX_BUSY;
    if (budget > 0) budget--;
    if (nsent < 256) {
        memcpy(sent[nsent].d, data, len);
        sent[nsent].n = len;
        sent[nsent].rsp = with_response;
    }
    nsent++;
    return BLELINK_TX_OK;
}
void demo_transport_disconnect(void) { disconnects++; }
uint32_t demo_transport_now_ms(void) { return now; }
void demo_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("    demo: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* ---- scripted server --------------------------------------------------------------------------- */
static void link_up(uint16_t mtu)
{
    demo_evt_t e;
    memset(&e, 0, sizeof(e));
    e.kind = DEMO_EVT_LINK_UP;
    e.mtu = mtu;
    push(&e);
}
static void simple(uint8_t kind)
{
    demo_evt_t e;
    memset(&e, 0, sizeof(e));
    e.kind = kind;
    push(&e);
}
static void notify(const uint8_t *p, size_t n)
{
    demo_evt_t e;
    memset(&e, 0, sizeof(e));
    e.kind = DEMO_EVT_PACKET;
    e.len = (uint16_t)n;
    memcpy(e.data, p, n);
    push(&e);
}
static void srv_hello(void)
{
    uint8_t p[12];
    blelink_hello_t h = {1, 20, 16777216u, 1, 0, 0};
    notify(p, blelink_encode_hello(p, 0, &h));
    srv_seq = 1;
}
static void srv_ack(uint16_t id)
{
    uint8_t p[5];
    notify(p, blelink_encode_ack(p, srv_seq++, id, 0));
}
static void srv_message(uint16_t id, const char *s)
{
    uint8_t p[20];
    uint32_t len = (uint32_t)strlen(s), off;
    uint16_t n = (uint16_t)(len < 8 ? len : 8);
    notify(p, blelink_encode_start(p, srv_seq++, id, len, blelink_crc32(0, (const uint8_t *)s, len),
                                   (const uint8_t *)s, n));
    for (off = n; off < len; off += n) {
        n = (uint16_t)(len - off < 16 ? len - off : 16);
        notify(p, blelink_encode_data(p, srv_seq++, id, (const uint8_t *)s + off, n));
    }
}
static void run(int steps)
{
    while (steps-- > 0) demo_step(20);
}

/* Reassembles client message `id` from sent[from..]; returns its length (-1 if no MSG_START). */
static int client_message(int from, uint16_t id, uint8_t *out)
{
    int i, len = -1;
    for (i = from; i < nsent; i++) {
        const pkt_t *k = &sent[i];
        if (k->d[0] == BLELINK_PKT_MSG_START && blelink_get_u16(k->d + 2) == id) {
            len = k->n - 12;
            memcpy(out, k->d + 12, (size_t)len);
        } else if (len >= 0 && k->d[0] == BLELINK_PKT_MSG_DATA && blelink_get_u16(k->d + 2) == id) {
            memcpy(out + len, k->d + 4, k->n - 4u);
            len += k->n - 4;
        }
    }
    return len;
}
static int count_type(int from, uint8_t type)
{
    int i, c = 0;
    for (i = from; i < nsent; i++)
        if (sent[i].d[0] == type) c++;
    return c;
}

int main(void)
{
    uint8_t buf[256];
    int n0, len;
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(demo_init() == 0);

    /* link up at ATT_MTU 23: HELLO, written with response */
    link_up(23);
    run(1);
    CHECK(nsent == 1 && sent[0].d[0] == BLELINK_PKT_HELLO && sent[0].n == 12 && sent[0].rsp == 1);
    CHECK(blelink_get_u16(sent[0].d + 3) == DEMO_MAX_PACKET && sent[0].d[9] == 1); /* maxPacket, window 1 */

    /* server HELLO: READY, greeting goes out as msg 0 at packetSize 20 */
    srv_hello();
    run(2);
    len = client_message(1, 0, buf);
    CHECK(len == (int)strlen(GREETING) && memcmp(buf, GREETING, (size_t)len) == 0);
    CHECK(sent[nsent - 1].n <= 20 && sent[nsent - 1].rsp == 0);
    srv_ack(0);
    run(1);

    /* server message: ACK 0, echoed back as msg 1 with identical bytes */
    n0 = nsent;
    srv_message(0, "ping from the server, 0123456789");
    run(6);
    CHECK(count_type(n0, BLELINK_PKT_ACK) == 1);
    len = client_message(n0, 1, buf);
    CHECK(len == 32 && memcmp(buf, "ping from the server, 0123456789", 32) == 0);
    srv_ack(1);
    run(1);

    /* the server echoes our echo / greeting back: no ping-pong */
    n0 = nsent;
    srv_message(1, "ping from the server, 0123456789");
    srv_message(2, GREETING);
    run(8);
    CHECK(count_type(n0, BLELINK_PKT_ACK) == 2 && count_type(n0, BLELINK_PKT_MSG_START) == 0);

    /* flow control: stack busy, then TX_READY resumes the echo */
    n0 = nsent;
    budget = 0;
    srv_message(3, "flow control test message");
    run(4);
    CHECK(nsent == n0);
    budget = -1;
    simple(DEMO_EVT_TX_READY);
    run(1);
    len = client_message(n0, 2, buf);
    CHECK(count_type(n0, BLELINK_PKT_ACK) == 1 && len == 25 && memcmp(buf, "flow control test message", 25) == 0);

    /* disconnect before the ACK, reconnect: new session, HELLO again, greeting is msg 0 again */
    simple(DEMO_EVT_LINK_DOWN);
    run(1);
    n0 = nsent;
    link_up(247);
    run(1);
    CHECK(nsent == n0 + 1 && sent[n0].d[0] == BLELINK_PKT_HELLO);
    srv_hello();
    run(1);
    CHECK(client_message(n0, 0, buf) == (int)strlen(GREETING));

    /* no server HELLO at all: after 1 + 3 HELLOs the demo asks the port to disconnect */
    simple(DEMO_EVT_LINK_DOWN);
    link_up(23);
    run(2);
    n0 = nsent;
    run(4 * 10000 / 20 + 10); /* 4 x T (10 s) of 20 ms steps */
    CHECK(count_type(n0, BLELINK_PKT_HELLO) == 3 && disconnects == 1);

    printf("demo: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
