/*
 * BleLink demo for ESP32 with ESP-IDF (v5.x) and the NimBLE host, GATT central role.
 *
 * Transport adapter (demo_transport.h) for NimBLE: scan for "BLE-TestServer" or the BleLink service
 * UUID, connect, exchange MTU (preferred 247), discover the BleLink service, RX, TX and the TX CCCD,
 * enable notifications, then hand over to the shared demo (examples/common/demo_app.c).
 *
 * Threading: NimBLE callbacks run in the NimBLE host task. They only copy data into a FreeRTOS queue;
 * the demo (app_main task) is the only caller of blelink_*. Writes are issued from the app task.
 * Flow control: ble_gattc_write_no_rsp_flat() fails with BLE_HS_ENOMEM / BLE_HS_EBUSY when the stack
 * has no buffer; the adapter reports BLELINK_TX_BUSY and retries after a short delay (PROTOCOL.md 6.2).
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

#include "blelink.h"
#include "demo_transport.h"

static const char *TAG = "blelink_port";

/* 7b1e10xx-5a6c-4f0e-9d3b-2c1a8e4f6b01; NimBLE stores 128-bit UUIDs little-endian. */
#define BLELINK_UUID(x)                                                                                              \
    BLE_UUID128_INIT(0x01, 0x6b, 0x4f, 0x8e, 0x1a, 0x2c, 0x3b, 0x9d, 0x0e, 0x4f, 0x6c, 0x5a, (x), 0x10, 0x1e, 0x7b)
static const ble_uuid128_t s_svc_uuid = BLELINK_UUID(0x00);
static const ble_uuid128_t s_rx_uuid = BLELINK_UUID(0x01);
static const ble_uuid128_t s_tx_uuid = BLELINK_UUID(0x02);

#define QUEUE_DEPTH 16 /* 16 x ~250 B; a full queue drops a notification = seq gap, repaired by RESUME */
#define BUSY_RETRY_MS 5

static QueueHandle_t s_queue;
static uint8_t s_own_addr_type;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile uint16_t s_rx_handle, s_tx_handle;
static uint16_t s_svc_start, s_svc_end, s_cccd_handle;
static volatile bool s_tx_blocked;
static demo_evt_t s_host_evt; /* only used from the NimBLE host task */

static int gap_event(struct ble_gap_event *event, void *arg);

static void post(uint8_t kind, uint16_t mtu, const struct os_mbuf *om)
{
    s_host_evt.kind = kind;
    s_host_evt.mtu = mtu;
    s_host_evt.len = 0;
    if (om) {
        uint16_t len = OS_MBUF_PKTLEN(om);
        if (len > sizeof(s_host_evt.data)) {
            ESP_LOGW(TAG, "notification of %u bytes > maxPacket: dropped", (unsigned)len);
            return;
        }
        os_mbuf_copydata(om, 0, len, s_host_evt.data);
        s_host_evt.len = len;
    }
    if (xQueueSend(s_queue, &s_host_evt, 0) != pdTRUE) ESP_LOGW(TAG, "event queue full: event %u dropped", kind);
}

static void fail_link(const char *what, int rc)
{
    ESP_LOGE(TAG, "%s failed (rc %d): disconnecting", what, rc);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

/* ---- scan ---------------------------------------------------------------------------------------- */
static void start_scan(void)
{
    struct ble_gap_disc_params p;
    int rc;
    memset(&p, 0, sizeof(p));
    p.filter_duplicates = 1;
    p.passive = 0; /* active: the name may be in the scan response */
    rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
    else ESP_LOGI(TAG, "scanning for \"%s\"", DEMO_SERVER_NAME);
}

static bool adv_matches(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields f;
    int i;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) return false;
    if (f.name && f.name_len == strlen(DEMO_SERVER_NAME) && memcmp(f.name, DEMO_SERVER_NAME, f.name_len) == 0)
        return true;
    for (i = 0; i < f.num_uuids128; i++)
        if (ble_uuid_cmp(&f.uuids128[i].u, &s_svc_uuid.u) == 0) return true;
    return false;
}

/* ---- connect -> MTU -> service -> characteristics -> CCCD -> notifications on -------------------- */
static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    (void)arg;
    if (err->status != 0) {
        fail_link("enable TX notifications", err->status);
        return 0;
    }
    post(DEMO_EVT_LINK_UP, ble_att_mtu(conn), NULL);
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    static const uint8_t enable_notify[2] = {0x01, 0x00};
    int rc;
    (void)chr_val_handle;
    (void)arg;
    if (err->status == 0) {
        /* the first CCCD after the TX value handle belongs to TX */
        if (!s_cccd_handle && ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0)
            s_cccd_handle = dsc->handle;
        return 0;
    }
    if (err->status != BLE_HS_EDONE) {
        fail_link("descriptor discovery", err->status);
        return 0;
    }
    if (!s_cccd_handle) {
        fail_link("TX has no CCCD", 0);
        return 0;
    }
    /* PROTOCOL.md 1: notifications on TX before HELLO */
    rc = ble_gattc_write_flat(conn, s_cccd_handle, enable_notify, sizeof(enable_notify), on_cccd_written, NULL);
    if (rc) fail_link("CCCD write", rc);
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    int rc;
    (void)arg;
    if (err->status == 0) {
        if (ble_uuid_cmp(&chr->uuid.u, &s_rx_uuid.u) == 0) s_rx_handle = chr->val_handle;
        if (ble_uuid_cmp(&chr->uuid.u, &s_tx_uuid.u) == 0) s_tx_handle = chr->val_handle;
        return 0;
    }
    if (err->status != BLE_HS_EDONE) {
        fail_link("characteristic discovery", err->status);
        return 0;
    }
    if (!s_rx_handle || !s_tx_handle) {
        fail_link("RX/TX characteristic not found", 0);
        return 0;
    }
    rc = ble_gattc_disc_all_dscs(conn, s_tx_handle, s_svc_end, on_dsc, NULL);
    if (rc) fail_link("ble_gattc_disc_all_dscs", rc);
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    int rc;
    (void)arg;
    if (err->status == 0) {
        s_svc_start = svc->start_handle;
        s_svc_end = svc->end_handle;
        return 0;
    }
    if (err->status != BLE_HS_EDONE) {
        fail_link("service discovery", err->status);
        return 0;
    }
    if (!s_svc_start) {
        fail_link("BleLink service not found", 0);
        return 0;
    }
    rc = ble_gattc_disc_all_chrs(conn, s_svc_start, s_svc_end, on_chr, NULL);
    if (rc) fail_link("ble_gattc_disc_all_chrs", rc);
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    int rc;
    (void)arg;
    if (err->status == 0) ESP_LOGI(TAG, "ATT_MTU %u", (unsigned)mtu);
    else ESP_LOGW(TAG, "MTU exchange failed (%d): continuing with ATT_MTU %u", err->status, ble_att_mtu(conn));
    rc = ble_gattc_disc_svc_by_uuid(conn, &s_svc_uuid.u, on_svc, NULL);
    if (rc) fail_link("ble_gattc_disc_svc_by_uuid", rc);
    return 0;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    int rc;
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (s_conn != BLE_HS_CONN_HANDLE_NONE || !adv_matches(&event->disc)) return 0;
        ble_gap_disc_cancel();
        rc = ble_gap_connect(s_own_addr_type, &event->disc.addr, 30000, NULL, gap_event, NULL);
        if (rc) {
            ESP_LOGE(TAG, "ble_gap_connect rc=%d", rc);
            start_scan();
        }
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connect failed (%d)", event->connect.status);
            start_scan();
            return 0;
        }
        s_conn = event->connect.conn_handle;
        s_svc_start = s_svc_end = s_cccd_handle = 0;
        s_rx_handle = s_tx_handle = 0;
        ESP_LOGI(TAG, "connected");
        rc = ble_gattc_exchange_mtu(s_conn, on_mtu, NULL);
        if (rc) fail_link("ble_gattc_exchange_mtu", rc);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason 0x%x)", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_rx_handle = s_tx_handle = 0;
        post(DEMO_EVT_LINK_DOWN, 0, NULL);
        start_scan();
        return 0;
    case BLE_GAP_EVENT_MTU:
        post(DEMO_EVT_MTU, event->mtu.value, NULL);
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX:
        /* the stack frees notify_rx.om after we return: copy it */
        if (event->notify_rx.conn_handle == s_conn && event->notify_rx.attr_handle == s_tx_handle &&
            !event->notify_rx.indication)
            post(DEMO_EVT_PACKET, 0, event->notify_rx.om);
        return 0;
    default: return 0;
    }
}

/* ---- demo_transport.h ---------------------------------------------------------------------------- */
static int on_write_rsp(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (err->status != 0) ESP_LOGW(TAG, "write with response failed (%d)", err->status);
    return 0;
}

int demo_transport_send(void *user, const uint8_t *data, uint16_t len, int with_response)
{
    uint16_t conn = s_conn, rx = s_rx_handle;
    int rc;
    (void)user;
    if (conn == BLE_HS_CONN_HANDLE_NONE || !rx) {
        s_tx_blocked = true; /* LINK_DOWN is on its way */
        return BLELINK_TX_BUSY;
    }
    if (with_response) /* HELLO (PROTOCOL.md 1): the data is copied into an mbuf by NimBLE */
        rc = ble_gattc_write_flat(conn, rx, data, len, on_write_rsp, NULL);
    else
        rc = ble_gattc_write_no_rsp_flat(conn, rx, data, len);
    if (rc == 0) return BLELINK_TX_OK;
    if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) ESP_LOGW(TAG, "write rc=%d", rc);
    s_tx_blocked = true;
    return BLELINK_TX_BUSY;
}

int demo_transport_wait(demo_evt_t *e, uint32_t timeout_ms)
{
    bool blocked = s_tx_blocked;
    TickType_t ticks = pdMS_TO_TICKS(blocked && timeout_ms > BUSY_RETRY_MS ? BUSY_RETRY_MS : timeout_ms);
    if (ticks == 0) ticks = 1;
    if (xQueueReceive(s_queue, e, ticks) == pdTRUE) return 1;
    if (blocked) { /* NimBLE has no "buffer free" event for write without response: retry */
        s_tx_blocked = false;
        memset(e, 0, sizeof(*e));
        e->kind = DEMO_EVT_TX_READY;
        return 1;
    }
    return 0;
}

void demo_transport_disconnect(void)
{
    uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

uint32_t demo_transport_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void demo_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("I (%lu) blelink: ", (unsigned long)demo_transport_now_ms());
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc) {
        ESP_LOGE(TAG, "no usable BLE address (rc %d)", rc);
        return;
    }
    start_scan();
}

static void on_reset(int reason) { ESP_LOGE(TAG, "NimBLE host reset, reason %d", reason); }

static void host_task(void *param)
{
    (void)param;
    nimble_port_run(); /* returns only after nimble_port_stop() */
    nimble_port_freertos_deinit();
}

int demo_transport_start(void)
{
    esp_err_t err = nvs_flash_init(); /* the BLE controller stores calibration data in NVS */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(demo_evt_t));
    if (!s_queue) return -1;
    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return -1;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_att_set_preferred_mtu(DEMO_MAX_PACKET + 3);
    nimble_port_freertos_init(host_task);
    return 0;
}

void app_main(void)
{
    if (demo_transport_start() == 0) demo_run();
}
