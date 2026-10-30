# BleLink C ABI (API version 3)

`BleLink_*` in `include/IBleServer.h` exposes the BleLink v1 protocol (`docs/PROTOCOL.md`) to C and C#
callers. The implementation is `src/common/BleLinkAbi.cpp`; it runs `BleLink::Engine` (`docs/BLELINK_ENGINE.md`)
in the server role on top of a `BleServer_*` handle. The Unity wrapper is the `BleLink` class in
`unity/BleServer.cs`. `BleServer_GetApiVersion()` returns 3 and `BleServer_GetBuildInfo()` contains `api=3`.

## What it does

- Registers one GATT service `7b1e1000-5a6c-4f0e-9d3b-2c1a8e4f6b01` with RX `...1001` (write, write without
  response), TX `...1002` (notify) and INFO `...1003` (read, 10-byte value with our limits).
- Opens a session per central when it subscribes to TX (HELLO handshake). `PEER_READY` reports the
  negotiated values; `BleLink_Send` is allowed from then on.
- Sends messages of 0 bytes up to the peer's `maxMessage` to one central through `IBleServer::NotifyPeer`
  (one notification to one central). A `Busy` result is flow control: the engine waits for the backend's
  ready-to-send signal; nothing is dropped. Packet size follows the central's MTU, also when it changes
  mid-session.
- Receives messages up to our `maxMessage` and delivers them after the CRC check.

## Lifecycle

1. `BleServer_Create`, `BleServer_Initialize`.
2. `BleLink_Create(server, config)` (config may be NULL = defaults). At most one link per server.
3. Add your own services, then `BleServer_StartAdvertising` (the BleLink service must exist before
   advertising starts).
4. Poll events, send messages.
5. `BleLink_Destroy(link)` removes the service. `BleServer_Shutdown` / `BleServer_Destroy` shut the link down
   first: messages not yet acknowledged fail with reason `SHUTDOWN`, and those events stay pollable. After
   `BleServer_Initialize` again, the old link stays dead; destroy it and create a new one.

Every entry point validates its handle (NULL, unknown, destroyed, or a server handle passed as a link) and
returns an error instead of crashing. Each call is safe from any thread, including concurrently with
`BleLink_Destroy`, and no C++ exception crosses the ABI.

## Configuration (`BleLinkConfigC`, 24 bytes)

| Field | Range | 0 means |
|---|---|---|
| `size` | `sizeof(BleLinkConfigC)` or an older, smaller value | required; fields past `size` take defaults |
| `maxMessage` | 1..0x7FFFFFFF | 16 MiB |
| `window` | 1..255 | 4 |
| `maxPacket` | 20..512 | 512 |
| `timeoutMs` | 1..63750 (rounded up to 250 ms) | 10 s |
| `flags` | `BLELINK_FLAG_NO_PROGRESS`, `BLELINK_FLAG_ENCRYPTED` | none; unknown bits are rejected |

`BleLink_Create` returns NULL for an invalid config, an uninitialized server or a second link.
`BleServer_GetLastError(server)` gives the reason.

## Events (polled, no callbacks)

```c
BleLinkEventC ev; ev.size = sizeof(ev);
for (;;) {
    int n = BleLink_GetEventPayloadSize(link);   // -1 = nothing queued
    if (n < 0) break;
    uint8_t* buf = n ? malloc(n) : NULL;
    if (BleLink_PollEvent(link, &ev, buf, n) == 1) handle(&ev, buf);
    free(buf);
}
```

| Type | Meaning | Fields |
|---|---|---|
| `PEER_READY` (1) | session READY (also after a reset) | `info` |
| `PEER_GONE` (2) | session ended | `reason` |
| `MESSAGE_RECEIVED` (3) | complete, verified message; payload = message | `dataLength`, `wireMsgId` |
| `MESSAGE_SENT` (4) | the central ACKed the message | `messageId` |
| `MESSAGE_FAILED` (5) | the message was not delivered | `messageId`, `reason`, `ackStatus` |
| `PROGRESS` (6) | every 64 KiB for messages of 64 KiB or more | `progressKind`, `bytes`, `total`, `messageId` (0 = incoming) |

Every event carries `deviceId` (UTF-8, NUL-terminated, at most 255 bytes) and `deviceIdLength`.
`BleLink_PollEvent` returns 1 (taken), 0 (none), `-BufferTooSmall` (the event stays queued, `ev->dataLength`
says how much is needed) or `-InvalidParameter`. Poll from one thread. Events queue without bound until
polled, but the protocol runs on a native worker thread, so the poll rate affects latency only.

## Errors

`BleLink_Send` returns `Success`, `InvalidParameter`, `PeerNotReady` (10, no READY session),
`MessageTooLarge` (11, above the peer's `maxMessage`) or `NotInitialized` (link shut down).
`BufferTooSmall` (12) only comes from `BleLink_PollEvent`. These three codes are new in API v3; older entry
points never return them.

## Backend notes

- A TX unsubscribe while the central stays connected closes the session; a new subscription opens a new one.
- macOS reports an unsubscribe as a disconnect. The link closes the session only for a TX unsubscribe or when
  the central is no longer in `GetConnectedDevices()`, so unsubscribing an unrelated characteristic keeps
  the session.
- Do not install your own read/write callbacks on the BleLink characteristics.

## Deprecated

`BleChunker_*` / `BleMessageChunker` (fixed 256-byte chunks to every central, ATT_MTU >= 259, no flow control)
keeps working unchanged. Define `BLE_WARN_DEPRECATED_CHUNKER` to get C++ warnings. In C#, `BleChunker`,
`BleServer.SendLargeMessage`, `BleServer.Chunker` and `BleServer.RetransmitTimedOut` are `[Obsolete]`.

## Tests

`tests/test_blelink_abi.cpp` (`ctest -R blelink_abi`) drives the ABI on the in-memory fake backend with a
client-role engine as the central. It covers handle validation, config validation, the INFO value,
round trips of 0 B to 300 KB in both directions, progress, `BufferTooSmall`, flow control with a
3-notification queue at a 20-byte MTU, MTU growth, cancel, disconnect, unsubscribe, the macOS
unrelated-unsubscribe case, both `maxMessage` limits, shutdown and re-initialize, destroy order, and concurrent
send/poll/destroy. Real Bluetooth and Unity are not tested in CI.
