# BleLink v1 MCU client (C99)

GATT-central side of the BleLink message protocol (`docs/PROTOCOL.md`) for microcontrollers.
It was written from the spec only and is checked against `docs/protocol-test-vectors.json`.

- Portable C99: no malloc, no OS, no floating point. The context and all buffers belong to the caller.
- Messages of any size in both directions, with flow control (`BLELINK_TX_BUSY` + `blelink_on_ready()`),
  ACK, RESUME/MSG_RESEND repair, timeouts and duplicate memory.
- Receive is either **buffered** (a RAM buffer split between the receive slots) or **streaming**
  (`RX_START` / `RX_CHUNK` / `RX_DONE`, data in offset order, CRC result at the end). Streaming suits
  writing to flash.
- Incremental CRC32 in three sizes: bitwise, 16-entry nibble table (64 B), or 256-entry table (1 KB).

## Layout

| Path | Content |
|---|---|
| `include/blelink_codec.h`, `src/blelink_codec.c` | packet encode/decode, CRC32 |
| `include/blelink.h`, `src/blelink.c` | session: HELLO, send/receive, repair, timers |
| `examples/common/` | demo app (HELLO, greeting, echo) behind a small transport interface |
| `ports/esp32-nimble/` | ESP-IDF v5.x project, NimBLE central |
| `ports/zephyr/` | Zephyr / nRF Connect SDK app, nRF52840 DK |
| `tests/` | host tests: vectors, session behaviour, demo app against a fake server |

## Size (Cortex-M4, `-Os`, library only, text + data)

| CRC32 | flash |
|---|---|
| `BLELINK_CRC32_IMPL=0` bitwise (default) | 6.1 KB |
| `BLELINK_CRC32_IMPL=1` nibble table | 6.2 KB |
| `BLELINK_CRC32_IMPL=2` byte table | 7.1 KB |

These numbers come from clang `--target=thumbv7em-none-eabi`. CI measures with `arm-none-eabi-gcc`
and fails at 8 KB or more. With the default sizing, `blelink_ctx_t` is 252 bytes of RAM, plus your
`tx_packet` buffer and, in buffered mode, the receive buffer.

Compile-time sizing: `BLELINK_MAX_RX_WINDOW` (1), `BLELINK_MAX_TX` (2), `BLELINK_CTRL_QUEUE` (4),
`BLELINK_DUP_MEMORY`, `BLELINK_UNKNOWN_MEMORY`. See `include/blelink.h`. Use the same values in every
translation unit.

## Usage

```c
static blelink_ctx_t link;
static uint8_t pkt[244];

static int on_event(void *user, const blelink_event_t *e) {
    if (e->type == BLELINK_EVT_RX_CHUNK) flash_write(e->offset, e->data, e->len);
    if (e->type == BLELINK_EVT_RX_DONE)  return e->crc_ok ? commit() : discard();
    if (e->type == BLELINK_EVT_PEER_READY) blelink_send(&link, (const uint8_t *)"hi", 2, NULL);
    return 0;
}

blelink_config_t cfg = {0};
blelink_buffers_t bufs = {0};               /* rx_buffer NULL = streaming receive */
cfg.max_packet = 244;  cfg.window = 1;  cfg.timeout = 0;   /* 0 = 10 s */
cfg.send_packet = my_gatt_write;  cfg.on_event = on_event;
bufs.tx_packet = pkt;  bufs.tx_packet_size = sizeof pkt;
blelink_init(&link, &cfg, &bufs);

/* port, all on ONE task: */
blelink_connect(&link, att_mtu - 3);        /* TX notifications enabled: sends HELLO */
blelink_on_packet(&link, data, len);        /* each TX notification, copied out of the BLE callback */
blelink_on_ready(&link);                    /* the stack has room again after BLELINK_TX_BUSY */
blelink_poll(&link, now_ms);                /* every 10-50 ms */
blelink_on_disconnected(&link);
```

`blelink_*` is not thread-safe. BLE host callbacks should only queue data. Both ports show the pattern.

## Build and test (host)

```sh
cmake -S clients/mcu/blelink -B build-blelink && cmake --build build-blelink
ctest --test-dir build-blelink --output-on-failure
# ASan + UBSan:
cmake -S clients/mcu/blelink -B build-san -DBLELINK_SANITIZERS=ON && cmake --build build-san && ctest --test-dir build-san
```

The tests are built in three sizing/CRC configurations, plus a test of the demo app.
`tests/gen_vectors_h.py` turns the JSON vectors into a C header at build time (needs Python 3).

## Examples

- ESP32: `cd ports/esp32-nimble && idf.py set-target esp32 && idf.py build flash monitor`
- nRF52840 DK: `west build -b nrf52840dk/nrf52840 clients/mcu/blelink/ports/zephyr && west flash`

Both scan for `BLE-TestServer` or the BleLink service UUID, connect, request ATT_MTU 247, enable TX
notifications, send HELLO and a greeting, then echo every message they receive. See NOTES.md for what
has not been tested on hardware.
