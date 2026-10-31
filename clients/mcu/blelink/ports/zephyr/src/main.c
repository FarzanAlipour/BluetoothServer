/*
 * BleLink demo for Zephyr / nRF Connect SDK (tested target: nRF52840 DK), GATT central role.
 *
 * Transport adapter (demo_transport.h) for the Zephyr Bluetooth host: scan for "BLE-TestServer" or
 * the BleLink service UUID, connect, exchange MTU (247 with prj.conf), discover the service, RX and
 * TX, subscribe to TX notifications (CCCD found by CONFIG_BT_GATT_AUTO_DISCOVER_CCC), then hand over
 * to the shared demo (examples/common/demo_app.c) running in the main thread.
 *
 * Threading: Bluetooth callbacks only copy into a k_msgq; the main thread is the only caller of
 * blelink_*. Flow control: a semaphore counts outstanding write-without-response packets and is
 * given back in the bt_gatt_write_without_response_cb() completion callback (PROTOCOL.md 6.2), so the
 * write never blocks waiting for an ATT buffer.
 */
#include <stdarg.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "blelink.h"
#include "demo_transport.h"

/* 7b1e10xx-5a6c-4f0e-9d3b-2c1a8e4f6b01 */
#define BLELINK_UUID_VAL(x) BT_UUID_128_ENCODE(0x7b1e1000 + (x), 0x5a6c, 0x4f0e, 0x9d3b, 0x2c1a8e4f6b01ULL)
static const struct bt_uuid_128 s_svc_uuid = BT_UUID_INIT_128(BLELINK_UUID_VAL(0));
static const struct bt_uuid_128 s_rx_uuid = BT_UUID_INIT_128(BLELINK_UUID_VAL(1));
static const struct bt_uuid_128 s_tx_uuid = BT_UUID_INIT_128(BLELINK_UUID_VAL(2));

/* Packets handed to the stack and not yet completed. Keep it <= the ATT/ACL TX buffer count so that
 * bt_gatt_write_without_response_cb() never has to wait for a buffer. */
#ifndef DEMO_TX_CREDITS
#define DEMO_TX_CREDITS 2
#endif
#define BUSY_RETRY_MS 2

K_MSGQ_DEFINE(s_queue, sizeof(demo_evt_t), 16, 4);
K_SEM_DEFINE(s_tx_credits, DEMO_TX_CREDITS, DEMO_TX_CREDITS);

static struct bt_conn *volatile s_conn;
static volatile uint16_t s_rx_handle, s_tx_handle;
static uint16_t s_svc_end;
static atomic_t s_tx_blocked;
static atomic_t s_hello_busy;
static uint8_t s_hello_buf[BLELINK_HELLO_LEN];
static struct bt_gatt_exchange_params s_mtu_params;
static struct bt_gatt_discover_params s_disc_params;
static struct bt_gatt_discover_params s_ccc_disc_params;
static struct bt_gatt_subscribe_params s_sub_params;
static struct bt_gatt_write_params s_write_params;
static demo_evt_t s_rx_evt; /* used from the BT RX thread only */

static void start_scan(void);

static void post(uint8_t kind, uint16_t mtu)
{
    demo_evt_t e;
    memset(&e, 0, sizeof(e));
    e.kind = kind;
    e.mtu = mtu;
    if (k_msgq_put(&s_queue, &e, K_NO_WAIT)) printk("blelink: event queue full, event %u dropped\n", kind);
}

static void fail_link(struct bt_conn *conn, const char *what, int err)
{
    printk("blelink: %s failed (%d): disconnecting\n", what, err);
    bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

/* ---- scan ---------------------------------------------------------------------------------------- */
static bool ad_matches(struct bt_data *data, void *user_data)
{
    bool *found = user_data;
    size_t i;
    switch (data->type) {
    case BT_DATA_NAME_COMPLETE:
    case BT_DATA_NAME_SHORTENED:
        if (data->data_len == strlen(DEMO_SERVER_NAME) && memcmp(data->data, DEMO_SERVER_NAME, data->data_len) == 0)
            *found = true;
        break;
    case BT_DATA_UUID128_ALL:
    case BT_DATA_UUID128_SOME:
        for (i = 0; i + 16 <= data->data_len; i += 16)
            if (memcmp(data->data + i, s_svc_uuid.val, 16) == 0) *found = true;
        break;
    default: break;
    }
    return !*found; /* stop parsing once found */
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type, struct net_buf_simple *ad)
{
    bool found = false;
    struct bt_conn *conn;
    int err;
    (void)rssi;
    if (s_conn) return;
    if (type != BT_GAP_ADV_TYPE_ADV_IND && type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND &&
        type != BT_GAP_ADV_TYPE_SCAN_RSP)
        return;
    bt_data_parse(ad, ad_matches, &found);
    if (!found) return;
    if (bt_le_scan_stop()) return;
    err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_DEFAULT, &conn);
    if (err) {
        printk("blelink: bt_conn_le_create failed (%d)\n", err);
        start_scan();
        return;
    }
    s_conn = conn; /* our reference, dropped in disconnected() */
}

static void start_scan(void)
{
    int err = bt_le_scan_start(BT_LE_SCAN_ACTIVE, device_found);
    if (err && err != -EALREADY) printk("blelink: scan start failed (%d)\n", err);
    else printk("blelink: scanning for \"%s\"\n", DEMO_SERVER_NAME);
}

/* ---- connect -> MTU -> service -> characteristics -> subscribe ----------------------------------- */
static uint8_t on_notify(struct bt_conn *conn, struct bt_gatt_subscribe_params *params, const void *data,
                         uint16_t length)
{
    (void)conn;
    if (!data) { /* unsubscribed */
        params->value_handle = 0;
        return BT_GATT_ITER_STOP;
    }
    if (length > sizeof(s_rx_evt.data)) {
        printk("blelink: notification of %u bytes > maxPacket: dropped\n", length);
        return BT_GATT_ITER_CONTINUE;
    }
    s_rx_evt.kind = DEMO_EVT_PACKET;
    s_rx_evt.len = length;
    memcpy(s_rx_evt.data, data, length);
    /* a full queue drops the packet: a seq gap the protocol repairs with RESUME (PROTOCOL.md 8) */
    if (k_msgq_put(&s_queue, &s_rx_evt, K_NO_WAIT)) printk("blelink: queue full, notification dropped\n");
    return BT_GATT_ITER_CONTINUE;
}

static void on_subscribed(struct bt_conn *conn, uint8_t err, struct bt_gatt_subscribe_params *params)
{
    (void)params;
    if (err) {
        fail_link(conn, "enable TX notifications", err);
        return;
    }
    post(DEMO_EVT_LINK_UP, bt_gatt_get_mtu(conn));
}

static void subscribe_tx(struct bt_conn *conn)
{
    int err;
    memset(&s_sub_params, 0, sizeof(s_sub_params));
    s_sub_params.notify = on_notify;
    s_sub_params.subscribe = on_subscribed;
    s_sub_params.value = BT_GATT_CCC_NOTIFY;
    s_sub_params.value_handle = s_tx_handle;
    s_sub_params.ccc_handle = 0; /* discovered automatically in [value_handle, end_handle] */
    s_sub_params.end_handle = s_svc_end;
    s_sub_params.disc_params = &s_ccc_disc_params;
    atomic_set_bit(s_sub_params.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);
    err = bt_gatt_subscribe(conn, &s_sub_params);
    if (err && err != -EALREADY) fail_link(conn, "bt_gatt_subscribe", err);
}

static uint8_t on_discover(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           struct bt_gatt_discover_params *params)
{
    int err;
    if (!attr) { /* end of this discovery */
        if (params->type == BT_GATT_DISCOVER_PRIMARY) fail_link(conn, "BleLink service discovery", -ENOENT);
        else if (!s_rx_handle || !s_tx_handle) fail_link(conn, "RX/TX characteristic discovery", -ENOENT);
        else subscribe_tx(conn);
        return BT_GATT_ITER_STOP;
    }
    if (params->type == BT_GATT_DISCOVER_PRIMARY) {
        const struct bt_gatt_service_val *svc = attr->user_data;
        s_svc_end = svc->end_handle;
        params->uuid = NULL;
        params->start_handle = attr->handle + 1;
        params->end_handle = svc->end_handle;
        params->type = BT_GATT_DISCOVER_CHARACTERISTIC;
        err = bt_gatt_discover(conn, params);
        if (err) fail_link(conn, "characteristic discovery", err);
        return BT_GATT_ITER_STOP;
    }
    {
        const struct bt_gatt_chrc *chrc = attr->user_data;
        if (bt_uuid_cmp(chrc->uuid, &s_rx_uuid.uuid) == 0) s_rx_handle = chrc->value_handle;
        if (bt_uuid_cmp(chrc->uuid, &s_tx_uuid.uuid) == 0) s_tx_handle = chrc->value_handle;
    }
    return BT_GATT_ITER_CONTINUE;
}

static void discover_service(struct bt_conn *conn)
{
    int err;
    memset(&s_disc_params, 0, sizeof(s_disc_params));
    s_disc_params.uuid = &s_svc_uuid.uuid;
    s_disc_params.func = on_discover;
    s_disc_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    s_disc_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    s_disc_params.type = BT_GATT_DISCOVER_PRIMARY;
    err = bt_gatt_discover(conn, &s_disc_params);
    if (err) fail_link(conn, "service discovery", err);
}

static void on_mtu_exchanged(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params)
{
    (void)params;
    if (err) printk("blelink: MTU exchange failed (%u), continuing\n", err);
    printk("blelink: ATT_MTU %u\n", bt_gatt_get_mtu(conn));
    discover_service(conn);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (conn != s_conn) return;
    if (err) {
        printk("blelink: connect failed (0x%02x)\n", err);
        bt_conn_unref(s_conn);
        s_conn = NULL;
        start_scan();
        return;
    }
    printk("blelink: connected\n");
    s_rx_handle = s_tx_handle = 0;
    s_mtu_params.func = on_mtu_exchanged;
    if (bt_gatt_exchange_mtu(conn, &s_mtu_params)) discover_service(conn); /* already done / not possible */
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    unsigned i;
    if (conn != s_conn) return;
    printk("blelink: disconnected (0x%02x)\n", reason);
    s_rx_handle = s_tx_handle = 0;
    bt_conn_unref(s_conn);
    s_conn = NULL;
    /* completions for packets still queued in the stack will not come: restore all credits */
    k_sem_reset(&s_tx_credits);
    for (i = 0; i < DEMO_TX_CREDITS; i++) k_sem_give(&s_tx_credits);
    atomic_clear(&s_hello_busy);
    post(DEMO_EVT_LINK_DOWN, 0);
    start_scan();
}

BT_CONN_CB_DEFINE(s_conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

static void att_mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
    (void)tx;
    (void)rx;
    if (conn == s_conn) post(DEMO_EVT_MTU, bt_gatt_get_mtu(conn));
}

static struct bt_gatt_cb s_gatt_callbacks = {
    .att_mtu_updated = att_mtu_updated,
};

/* ---- demo_transport.h ---------------------------------------------------------------------------- */
static void on_write_done(struct bt_conn *conn, void *user_data)
{
    (void)conn;
    (void)user_data;
    k_sem_give(&s_tx_credits);
}

static void on_hello_written(struct bt_conn *conn, uint8_t err, struct bt_gatt_write_params *params)
{
    (void)conn;
    (void)params;
    if (err) printk("blelink: HELLO write failed, ATT error 0x%02x\n", err);
    atomic_clear(&s_hello_busy);
}

int demo_transport_send(void *user, const uint8_t *data, uint16_t len, int with_response)
{
    struct bt_conn *conn = s_conn;
    uint16_t rx = s_rx_handle;
    int err;
    (void)user;
    if (!conn || !rx) goto busy; /* LINK_DOWN is on its way */
    if (with_response) {
        /* HELLO (PROTOCOL.md 1); params and data must stay valid until the response */
        if (len > sizeof(s_hello_buf) || atomic_set(&s_hello_busy, 1)) goto busy;
        memcpy(s_hello_buf, data, len);
        s_write_params.func = on_hello_written;
        s_write_params.handle = rx;
        s_write_params.offset = 0;
        s_write_params.data = s_hello_buf;
        s_write_params.length = len;
        err = bt_gatt_write(conn, &s_write_params);
        if (err) {
            atomic_clear(&s_hello_busy);
            goto busy;
        }
        return BLELINK_TX_OK;
    }
    if (k_sem_take(&s_tx_credits, K_NO_WAIT)) goto busy;
    err = bt_gatt_write_without_response_cb(conn, rx, data, len, false, on_write_done, NULL);
    if (err) {
        k_sem_give(&s_tx_credits);
        goto busy;
    }
    return BLELINK_TX_OK;
busy:
    atomic_set(&s_tx_blocked, 1);
    return BLELINK_TX_BUSY;
}

int demo_transport_wait(demo_evt_t *e, uint32_t timeout_ms)
{
    bool blocked = atomic_get(&s_tx_blocked) != 0;
    uint32_t t = blocked && timeout_ms > BUSY_RETRY_MS ? BUSY_RETRY_MS : timeout_ms;
    if (k_msgq_get(&s_queue, e, K_MSEC(t)) == 0) return 1;
    if (blocked) { /* retry the send: credits came back, or the stack was briefly out of buffers */
        atomic_clear(&s_tx_blocked);
        memset(e, 0, sizeof(*e));
        e->kind = DEMO_EVT_TX_READY;
        return 1;
    }
    return 0;
}

void demo_transport_disconnect(void)
{
    struct bt_conn *conn = s_conn;
    if (conn) bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

uint32_t demo_transport_now_ms(void) { return k_uptime_get_32(); }

void demo_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printk("[%u] blelink: ", k_uptime_get_32());
    vprintk(fmt, ap);
    printk("\n");
    va_end(ap);
}

int demo_transport_start(void)
{
    int err = bt_enable(NULL);
    if (err) {
        printk("blelink: bt_enable failed (%d)\n", err);
        return err;
    }
    bt_gatt_cb_register(&s_gatt_callbacks);
    start_scan();
    return 0;
}

int main(void)
{
    if (demo_transport_start() == 0) demo_run();
    return 0;
}
