# BleLink v1 Android client (Kotlin)

Android central for the BleLink v1 message protocol (`docs/PROTOCOL.md`): messages of any size in both
directions to a BleLink server (the Unity desktop app, or `ble_test_server` once it speaks BleLink).

- `blelink/`: the library (minSdk 23, Kotlin, coroutines/Flow API).
- `sample/`: a minimal app that scans for the BleLink service, connects, sends a text or an N-KB blob and shows
  progress and a log.

## Layout

| Package | What | Needs Bluetooth |
|---------|------|-----------------|
| `codec` | `PacketCodec`, `Packet`, `InfoCodec`, `Crc32`: bytes <-> packets | no |
| `session` | `BleLinkSession`: HELLO, seq, chunking, ACK/RESUME/CANCEL/PROGRESS, timers. Single-threaded and passive (you `poll()` it). | no |
| `transport` | `PacketTransport` (interface) and `GattTransport` (BluetoothGatt) | `GattTransport` only |
| root | `BleLinkClient`: session + transport, thread-safe, `Flow` events | via the transport |

Codec and session are covered by JVM unit tests against `docs/protocol-test-vectors.json`; the client is tested
over a fake transport. See `NOTES.md` for what is and is not verified.

## Usage

```kotlin
val client = BleLinkClient(GattTransport(context, device))   // device: from your BLE scan
scope.launch {
    client.events.collect { e ->
        when (e) {
            is BleLinkEvent.PeerReady -> println("ready, ${e.sendPacketSize}-byte packets")
            is BleLinkEvent.Received -> println("got ${e.data.size} bytes")
            is BleLinkEvent.Sent -> println("message ${e.msgId} ACKed")
            is BleLinkEvent.Failed -> println("message ${e.msgId} failed: ${e.reason}")
            is BleLinkEvent.Progress -> println("${e.msgId}: ${e.bytes}/${e.total} (${e.kind})")
            is BleLinkEvent.Disconnected -> println("disconnected: ${e.reason}")
        }
    }
}
client.connect()                              // GATT connect, MTU 517, CCCD, HELLO
val server = client.awaitReady()              // suspends until the server's HELLO
val id = client.send("hello".toByteArray())   // -> msgId; outcome arrives as Sent / Failed
client.send(ByteArray(256 * 1024))            // any size up to server.maxMessage
client.disconnect()
```

(The same code is compiled in `sample/src/main/java/.../ReadmeExample.kt`.)

API summary:

- `connect()`, `awaitReady(): PeerLimits`, `disconnect()`, `resetSession()` (re-send HELLO).
- `send(bytes): Int` returns the msgId at once. Allowed while OPENING (it waits for READY). Messages beyond the
  server's `window` wait in a local queue. `cancel(msgId)`.
- `state: StateFlow<LinkState>` (DISCONNECTED, CONNECTING, OPENING, READY).
- `events: SharedFlow<BleLinkEvent>`: `PeerReady`, `Received`, `Sent`, `Failed(reason, ackStatus)`,
  `Progress(kind = SENT_TO_STACK | PEER_CONFIRMED | RECEIVING)`, `Disconnected`. Subscribe **before** `connect()`;
  the buffer is unbounded, so slow collectors do not lose events.
- `LocalLimits` (constructor): what we advertise in our HELLO (maxMessage, default 16 MiB; window, default 4;
  PROGRESS wanted; timeout). Incoming messages are buffered whole in RAM, so `maxMessage * window` bounds memory.
- Failure reasons: `DISCONNECTED, RESET, TIMEOUT, CRC, TOO_LARGE, BUSY, REJECTED, LINK_UNRELIABLE, CANCELLED`.
  A message that failed with `DISCONNECTED` after it was fully written is *in doubt* (PROTOCOL.md 11).

## What the GATT transport does

`connectGatt(TRANSPORT_LE)` -> `discoverServices` -> `requestMtu(517)` -> `requestConnectionPriority(HIGH)` ->
`setPreferredPhy(LE 2M)` (API 26+, only if `isLe2MPhySupported`) -> enable TX notifications
(`setCharacteristicNotification` + CCCD `writeDescriptor`) -> HELLO with response. Then:

- Strictly one GATT operation at a time on a private `HandlerThread`; binder callbacks are copied and posted to it.
  Each operation has a 10 s timeout (the connection is closed if the stack never answers).
- Data goes out as Write Without Response; the next write is issued only after `onCharacteristicWrite`
  (PROTOCOL.md 6.2). A refused write (`false`, or `ERROR_GATT_WRITE_REQUEST_BUSY` on API 33+) is retried every
  10 ms, up to 50 times, then dropped; the server's seq/RESUME logic repairs the lost packet.
- API 33+ uses `writeCharacteristic(c, value, type)`, `writeDescriptor(d, value)` and
  `onCharacteristicChanged(g, c, value)`; older APIs use the deprecated `value` setters/getters.
- `osLimit = mtu - 3` (clamped to 20..512); an `onMtuChanged` after setup only changes our send size.
- Connection priority drops to BALANCED when nothing is in flight.
- Any disconnect (including status 133/8) closes the `BluetoothGatt`. Reconnect by calling `connect()` again,
  ideally after a short delay; a new `GattTransport` is not needed.

## Permissions

The library manifest declares `BLUETOOTH_CONNECT` (and the legacy `BLUETOOTH` up to API 30). Scanning is the
app's job; the sample shows the full set:

| API level | Manifest | Runtime request |
|-----------|----------|-----------------|
| 31+ (Android 12+) | `BLUETOOTH_SCAN` (`usesPermissionFlags="neverForLocation"` if you do not derive location), `BLUETOOTH_CONNECT` | both |
| 29-30 | `BLUETOOTH`, `BLUETOOTH_ADMIN`, `ACCESS_FINE_LOCATION` (+ COARSE), all `maxSdkVersion="30"` | `ACCESS_FINE_LOCATION`; Location must also be switched on for scan results |
| 23-28 | as 29-30 | `ACCESS_COARSE_LOCATION` or FINE |

`GattTransport` throws nothing on a missing permission; it reports `Disconnected("missing Bluetooth permission ...")`.
For long transfers with the screen off, run the client from a foreground service (`connectedDevice` type on
API 34+); otherwise the system may drop the connection (PROTOCOL.md 14.3).

## Build and test

```sh
cd clients/android
./gradlew test                    # JVM unit tests (codec, session, client over a fake transport)
./gradlew :sample:assembleDebug   # sample APK: sample/build/outputs/apk/debug/
./gradlew lint
```

Needs JDK 17+ and an Android SDK with platform 35 (`ANDROID_HOME` or `local.properties` `sdk.dir`). CI runs the
same on `ubuntu-latest` (job `android-client` in `.github/workflows/ci.yml`).
