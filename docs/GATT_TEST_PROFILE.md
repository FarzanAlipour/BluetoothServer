# GATT test profile

The single description of the GATT layout used for manual and automated tests. Sources of truth in code:

- C++: `examples/ble_test_server/TestProfile.h` (used by the `ble_test_server` CLI and by the fake-backend
  scenario test `tests/test_scenario_gatt.cpp`).
- The **Unity sample** (`samples/UnityBleSample`, `Assets/Scripts/BleSampleGatt.cs`, PR #5) mirrors this
  profile with the same UUIDs and behaviour; only the advertised name differs (`BoostanBLE-Sample`). When the
  profile changes, update `TestProfile.h`, this file and `BleSampleGatt.cs` together.

All UUIDs are custom 128-bit UUIDs (not Bluetooth SIG numbers). Advertised name of the CLI: `BLE-TestServer`
(`--name` overrides it).

## Layout

Service `7b1e0000-5a6c-4f0e-9d3b-2c1a8e4f6b01`. Each characteristic changes only the `0000` part of the first
group; the rest is `-5a6c-4f0e-9d3b-2c1a8e4f6b01`.

| Name | UUID prefix | Properties | Behaviour |
|---|---|---|---|
| Read | `7b1e0001` | Read | `uptime=<s>s read#<n>`, e.g. `uptime=12.3s read#5`. Generated per read; `<n>` counts central reads (starts at 1, never resets while the server runs). |
| Write | `7b1e0002` | Write, Write without response | The bytes are logged as hex and as UTF-8. No reply. |
| Notify | `7b1e0003` | Read, Notify | `heartbeat #<N>` every `--heartbeat-ms` (default 1000 ms) while at least one central is connected; `manual #<N>` when the `n` command is typed. `N` counts from 1 per kind. |
| Echo | `7b1e0004` | Write, Write without response, Notify | Whatever is written comes back as a notification with identical bytes. An empty write is logged and not echoed. |
| Large | `7b1e0005` | Write, Write without response, Notify | `l` command: a 1000-byte message goes out as 5 `BleMessageChunker` chunks (256 B each: 4 x 243 payload bytes, the last 28 bytes) as notifications. A packet written by the central is fed to the chunker: an `02` ACK marks the chunk acknowledged; a `01` data chunk is reassembled and logged when complete. Unacknowledged chunks are re-sent after 3 s. |

### Values

- Heartbeat payload: ASCII `heartbeat #1`, `heartbeat #2`, ...
- Large message payload: byte *i* = `'A' + (i % 26)`, i.e. `ABCDEFGHIJKLMNOPQRSTUVWXYZABC...`, 1000 bytes.
- Chunk header (13 bytes, little-endian): `01` data | message id (4 B) | chunk index (4 B) | total chunks (4 B), then
  payload. For the 1000-byte message the total is `05 00 00 00`; chunk indexes are `00 00 00 00`, `01 00 00 00`, ...
- ACK packet (written by the central, 13 bytes): `02` | message id | chunk index | `00 00 00 00`.

### Large message and MTU

A chunk is 256 bytes, so it arrives intact only when the ATT MTU is at least **259** (request 517 in
nRF Connect). With the default MTU 23 the link truncates chunks; that is a client setup issue, not a server bug.

### Writing long values from the central

- **Write without response** (Write Command) carries at most **MTU-3** bytes (20 with the default MTU 23,
  514 with MTU 517). There is no fragmentation for it: a longer value is cut or refused by the client.
- For a value longer than MTU-3 use **Write request** in nRF Connect. The client then sends it as a *long
  write* (ATT Prepare Write fragments with offsets + Execute Write). The server assembles the fragments and
  calls the write callback **once with the full value**.
- Maximum value length is **512 bytes** (Bluetooth limit for an attribute). Longer values are rejected with
  *Invalid Attribute Value Length*; a fragment that leaves a gap is rejected with *Invalid Offset*. On an error
  nothing is applied. For bigger messages use the `BleMessageChunker` protocol (Large characteristic).

How each backend gets a long write (shared rules: `src/common/LongWriteAssembler.h`):

| Backend | Delivery | Handling |
|---|---|---|
| macOS (CoreBluetooth) | Execute Write = one `didReceiveWriteRequests:` call with one request per fragment (`offset` set) | Assembled per characteristic, all-or-nothing; exactly one `respondToRequest:` (first request); one callback per characteristic. |
| Linux (BlueZ) | BlueZ normally merges the fragments and calls `WriteValue` once (offset 0). Unmerged fragments come as separate `WriteValue` calls with the `offset` option | Each fragment is applied to the stored value at its offset; the callback gets the full value so far. |
| Windows (WinRT) | One `WriteRequested` per fragment, `GattWriteRequest.Offset()` set | Same as Linux. |

On Linux and Windows the server cannot see where an unmerged long write ends, so the write callback fires
once per fragment, each time with the value assembled so far (the last call carries the full value).

### What "connected" means per OS

The backends report a central differently: Linux and Windows report the connection itself; macOS
(CoreBluetooth) reports a central only when it **subscribes** to a notifying characteristic (and removes it when it
unsubscribes). The CLI logs these as `[connect]` / `[disconnect]` lines, and the heartbeat starts when the first
central is listed. There is no separate subscribe event in the public API.
