/*
 * Transport adapter interface used by the BleLink demo (examples/common/demo_app.c).
 *
 * A port (ESP-IDF NimBLE, Zephyr) implements these functions with its BLE stack. The port does the
 * Bluetooth part only: scan for "BLE-TestServer" (or the BleLink service UUID), connect, exchange the
 * MTU, discover RX/TX, enable TX notifications, and then report LINK_UP. Notifications and stack
 * signals are handed to the demo thread through a queue, so no blelink_* call ever runs inside a BLE
 * host callback (docs/PROTOCOL.md 14.4).
 */
#ifndef DEMO_TRANSPORT_H
#define DEMO_TRANSPORT_H

#include <stdint.h>

#ifndef DEMO_MAX_PACKET
#define DEMO_MAX_PACKET 244 /* ATT_MTU 247 - 3: what both example ports request */
#endif

#define DEMO_SERVER_NAME "BLE-TestServer"

typedef enum {
    DEMO_EVT_LINK_UP = 1, /* connected, MTU exchanged, notifications on: .mtu = ATT_MTU */
    DEMO_EVT_PACKET,      /* one TX notification in .data/.len */
    DEMO_EVT_MTU,         /* ATT_MTU changed: .mtu */
    DEMO_EVT_TX_READY,    /* the stack has room again after a BUSY send */
    DEMO_EVT_LINK_DOWN    /* disconnected; the port scans again by itself */
} demo_evt_kind_t;

typedef struct {
    uint8_t kind; /* demo_evt_kind_t */
    uint16_t mtu;
    uint16_t len;
    uint8_t data[DEMO_MAX_PACKET];
} demo_evt_t;

/* Initialise the stack and start scanning. Returns 0 on success. */
int demo_transport_start(void);

/* Wait up to timeout_ms for the next event. Returns 1 if *e was filled, 0 on timeout. */
int demo_transport_wait(demo_evt_t *e, uint32_t timeout_ms);

/* blelink_send_packet_cb: write one packet to RX. Returns BLELINK_TX_OK or BLELINK_TX_BUSY. */
int demo_transport_send(void *user, const uint8_t *data, uint16_t len, int with_response);

/* Drop the connection (used when the HELLO handshake fails). */
void demo_transport_disconnect(void);

/* Monotonic milliseconds (wraps; the library uses unsigned differences). */
uint32_t demo_transport_now_ms(void);

/* Log a line (printf-like). */
void demo_log(const char *fmt, ...);

/* The demo loop = demo_init() + demo_step(20) forever; never returns. Called by the port's
 * main()/app_main() after demo_transport_start(). demo_init/demo_step exist for the host test. */
void demo_run(void);
int demo_init(void);
void demo_step(uint32_t wait_ms);

#endif /* DEMO_TRANSPORT_H */
