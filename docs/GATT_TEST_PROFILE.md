# GATT test profile

The single description of the GATT layout used for manual and automated tests. Sources of truth in code:

- C++: `examples/ble_test_server/TestProfile.h` (used by the `ble_test_server` CLI and by the fake-backend
  scenario test `tests/test_scenario_gatt.cpp`).
- The **Unity sample** (`samples/UnityBleSample`, `Assets/Scripts/BleSampleGatt.cs`, PR #5) mirrors this
  profile with the same UUIDs and behaviour; only the advertised name differs (`BoostanBLE-Sample`). When the
  profile changes, update `TestProfile.h`, this file and `BleSampleGatt.cs` together. (PR #5 still has the
  retired Large characteristic `7b1e0005`; drop it there when that PR is updated.)

All UUIDs are custom 128-bit UUIDs (not Bluetooth SIG numbers). Advertised name of the CLI: `BLE-TestServer`
(`--name` overrides it).

The server exposes **two services**:

1. the **test profile** below: simple characteristics for smoke tests by hand (nRF Connect);
2. the **BleLink service** (`7b1e1000-...`): messages of any size in both directions, specified in
   [PROTOCOL.md](PROTOCOL.md) and exposed through the C ABI described in [BLELINK_ABI.md](BLELINK_ABI.md).

## Test profile

Service `7b1e0000-5a6c-4f0e-9d3b-2c1a8e4f6b01`. Each characteristic changes only the `0000` part of the first
group; the rest is `-5a6c-4f0e-9d3b-2c1a8e4f6b01`.

| Name | UUID prefix | Properties | Behaviour |
|---|---|---|---|
| Read | `7b1e0001` | Read | `uptime=<s>s read#<n>`, e.g. `uptime=12.3s read#5`. Generated per read; `<n>` counts central reads (starts at 1, never resets while the server runs). |
| Write | `7b1e0002` | Write, Write without response | The bytes are logged as hex and as UTF-8. No reply. |
| Notify | `7b1e0003` | Read, Notify | `heartbeat #<N>` every `--heartbeat-ms` (default 1000 ms) while at least one central is connected; `manual #<N>` when the `n` command is typed. `N` counts from 1 per kind. |
| Echo | `7b1e0004` | Write, Write without response, Notify | Whatever is written comes back as a notification with identical bytes. An empty write is logged and not echoed. |
| ~~Large~~ | `7b1e0005` | retired | The legacy `BleMessageChunker` characteristic (fixed 256-byte chunks, ATT MTU >= 259, one ACK per chunk) is no longer registered. Large messages use BleLink. The `BleChunker_*` C ABI still exists, deprecated. |

Heartbeat payload: ASCII `heartbeat #1`, `heartbeat #2`, ...

## BleLink service

| Name | UUID | Properties | Content |
|---|---|---|---|
| Service | `7b1e1000-5a6c-4f0e-9d3b-2c1a8e4f6b01` | | primary |
| RX | `7b1e1001-...` | Write, Write without response | one BleLink packet per write (client -> server) |
| TX | `7b1e1002-...` | Notify | one BleLink packet per notification (server -> client) |
| INFO | `7b1e1003-...` | Read | 10 bytes: our limits (`version, maxPacket u16, maxMessage u32, window, flags, timeout`) |

Server limits (BleLink defaults): maxMessage 16 MiB, window 4, maxPacket 512, PROGRESS wanted, timeout 10 s. The
INFO value is therefore `01 00 02 00 00 00 01 04 01 00`.

What the CLI does with it:

- A central that subscribes to TX and writes HELLO to RX gets a session (`[link] peer ... READY`, with the packet
  size the server sends with = min(the client's maxPacket, ATT MTU - 3)).
- `m <text>` sends `<text>` as one message to every READY peer; `m` alone sends an empty message.
- `b <size>` sends a generated blob (`b 1000`, `b 64k`, `b 1M`; k = 1024). Byte *i* = `(i * 7 + 3) mod 256`, the
  `payloadRule` of `docs/protocol-test-vectors.json` (the Python reference client uses the same rule).
- `p` lists peers: state, packet size, ATT MTU, messages in flight / queued / being received, the peer's limits.
- Every received message is logged with its size, `CRC ok` and its CRC-32, the first 64 bytes as text and a
  throughput. With `--echo-messages` it is sent back to the sender as a new message.
- Sent messages are logged when the peer ACKs them (`delivered (ACK ok)`, duration, KB/s) or fails
  (`[error] ... failed: <reason>`).

Throughput: outgoing = size / (time from `BleLink_Send` to the peer's ACK). Incoming = bytes / time between the
first PROGRESS event (at 64 KiB) and completion, so it is only shown for messages of 64 KiB or more.

## What "connected" means per OS

The backends report a central differently: Linux and Windows report the connection itself; macOS
(CoreBluetooth) reports a central only when it **subscribes** to a notifying characteristic (and removes it when it
unsubscribes). The CLI logs these as `[connect]` / `[disconnect]` lines, and the heartbeat starts when the first
central is listed. BleLink sessions follow the TX subscription (see BLELINK_ABI.md, backend notes).
