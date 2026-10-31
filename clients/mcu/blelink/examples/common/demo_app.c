/*
 * BleLink demo, shared by the ESP32 (NimBLE) and Zephyr ports.
 *
 * After the link is up: HELLO, send one greeting message, then echo every received message back to
 * the server (ble_test_server / the Unity app). Buffered receive, window 1, 2 KiB receive buffer.
 * All blelink_* calls happen on this one thread.
 */
#include <stdio.h>
#include <string.h>

#include "blelink.h"
#include "demo_transport.h"

#ifndef DEMO_BOARD_NAME
#define DEMO_BOARD_NAME "mcu"
#endif
#define DEMO_RX_BUFFER 2048u

static blelink_ctx_t s_link;
static uint8_t s_rx_buffer[DEMO_RX_BUFFER];
static uint8_t s_tx_packet[DEMO_MAX_PACKET];
static char s_greeting[64];
static uint8_t s_echo[DEMO_RX_BUFFER]; /* must stay unchanged until SENT/FAILED (blelink_send) */
static uint32_t s_greeting_len, s_echo_len;
static int s_echo_busy;
static uint16_t s_echo_id;

static int on_event(void *user, const blelink_event_t *e)
{
    (void)user;
    switch (e->type) {
    case BLELINK_EVT_PEER_READY: {
        uint16_t id;
        int n = snprintf(s_greeting, sizeof(s_greeting), "hello from %s", DEMO_BOARD_NAME);
        demo_log("ready%s: packetSize %u, timeout %lu ms", e->reset ? " (after reset)" : "",
                 (unsigned)blelink_packet_size(&s_link), (unsigned long)blelink_timeout_ms(&s_link));
        s_greeting_len = (uint32_t)n;
        s_echo_busy = 0; /* a reset failed every message in flight */
        s_echo_len = 0;
        if (blelink_send(&s_link, (const uint8_t *)s_greeting, (uint32_t)n, &id) == BLELINK_OK)
            demo_log("sent greeting as msg %u", (unsigned)id);
        break;
    }
    case BLELINK_EVT_RECEIVED:
        demo_log("received msg %u, %lu bytes", (unsigned)e->msg_id, (unsigned long)e->len);
        /* Loop guard against a server that echoes too: never echo our own greeting or our last echo. */
        if ((e->len == s_greeting_len && memcmp(e->data, s_greeting, e->len) == 0) ||
            (e->len == s_echo_len && memcmp(e->data, s_echo, e->len) == 0)) {
            demo_log("msg %u is our own data coming back: not echoing", (unsigned)e->msg_id);
            break;
        }
        if (s_echo_busy) {
            demo_log("previous echo still in flight: not echoing msg %u", (unsigned)e->msg_id);
            break;
        }
        memcpy(s_echo, e->data, e->len);
        s_echo_len = e->len;
        if (blelink_send(&s_link, s_echo, e->len, &s_echo_id) == BLELINK_OK) s_echo_busy = 1;
        break;
    case BLELINK_EVT_SENT:
        demo_log("msg %u acknowledged", (unsigned)e->msg_id);
        if (s_echo_busy && e->msg_id == s_echo_id) s_echo_busy = 0;
        break;
    case BLELINK_EVT_FAILED:
        demo_log("msg %u failed, reason %u", (unsigned)e->msg_id, (unsigned)e->reason);
        if (s_echo_busy && e->msg_id == s_echo_id) s_echo_busy = 0;
        break;
    case BLELINK_EVT_PROGRESS:
        demo_log("msg %u: server has %lu bytes", (unsigned)e->msg_id, (unsigned long)e->offset);
        break;
    case BLELINK_EVT_HANDSHAKE_FAILED:
        demo_log("no HELLO from the server: disconnecting");
        demo_transport_disconnect();
        break;
    case BLELINK_EVT_DISCONNECTED:
        demo_log("session closed");
        s_echo_busy = 0;
        break;
    default: break;
    }
    return 0; /* accept everything */
}

int demo_init(void)
{
    blelink_config_t cfg;
    blelink_buffers_t bufs;

    memset(&cfg, 0, sizeof(cfg));
    cfg.max_packet = DEMO_MAX_PACKET; /* what we can receive in one notification */
    cfg.window = 1;
    cfg.send_packet = demo_transport_send;
    cfg.on_event = on_event;
    memset(&bufs, 0, sizeof(bufs));
    bufs.rx_buffer = s_rx_buffer; /* buffered mode: maxMessage = 2048 */
    bufs.rx_buffer_size = sizeof(s_rx_buffer);
    bufs.tx_packet = s_tx_packet;
    bufs.tx_packet_size = sizeof(s_tx_packet);
    if (blelink_init(&s_link, &cfg, &bufs) != BLELINK_OK) {
        demo_log("blelink_init failed");
        return -1;
    }
    return 0;
}

void demo_step(uint32_t wait_ms)
{
    static demo_evt_t ev; /* ~250 bytes: keep it off small MCU stacks */
    if (demo_transport_wait(&ev, wait_ms)) {
        switch (ev.kind) {
        case DEMO_EVT_LINK_UP:
            demo_log("link up, ATT_MTU %u: sending HELLO", (unsigned)ev.mtu);
            blelink_poll(&s_link, demo_transport_now_ms());
            blelink_connect(&s_link, (uint16_t)(ev.mtu - 3u));
            break;
        case DEMO_EVT_PACKET: blelink_on_packet(&s_link, ev.data, ev.len); break;
        case DEMO_EVT_MTU: blelink_set_os_limit(&s_link, (uint16_t)(ev.mtu - 3u)); break;
        case DEMO_EVT_TX_READY: blelink_on_ready(&s_link); break;
        case DEMO_EVT_LINK_DOWN: blelink_on_disconnected(&s_link); break;
        default: break;
        }
    }
    blelink_poll(&s_link, demo_transport_now_ms());
}

void demo_run(void)
{
    if (demo_init() != 0) return;
    for (;;) demo_step(20);
}
