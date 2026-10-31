# BleLink for iOS / macOS (Swift)

Client (GATT central) for the BleLink v1 message protocol (`docs/PROTOCOL.md`). Sends and receives messages of any
size to and from the BluetoothServer (Unity desktop app or `ble_test_server`). Swift Package, iOS 13+ / macOS 11+.

| Product / target | What | Needs Bluetooth |
|------------------|------|-----------------|
| `BleLinkCore` | codec (`Packet`, `HelloBody`, `CRC32`) + session state machine (`BleLinkSession`) | no (pure Swift, unit-tested) |
| `BleLink` | CoreBluetooth transport `BleLinkClient` (re-exports `BleLinkCore`) | yes |
| `BleLinkSample` | minimal SwiftUI app: send text / N-KB blob with progress (`Sources/BleLinkSample/README.md`) | yes |

## Usage

```swift
import BleLink

let client = BleLinkClient()                       // events on the main queue by default
Task {
    for await event in client.events {
        switch event {
        case let .peerReady(peer, packetSize): print("ready, server window \(peer.window), \(packetSize) B/packet")
        case let .received(id, data):          print("got #\(id): \(data.count) B")
        case let .sent(id):                    print("#\(id) ACKed by the server")
        case let .failed(id, reason):          print("#\(id) failed: \(reason)")
        case let .progress(p):                 print("#\(p.msgId) \(Int(p.fraction * 100)) %")
        case .helloTimeout, .disconnected:     print("link down")
        }
    }
}
let server = try await client.connect()            // scan, connect, subscribe TX, HELLO
let id = try await client.send(Data("hello".utf8)) // returns the msgId at once; result comes as .sent / .failed
try await client.sendAndWait(Data(count: 64 * 1024)) // or wait for the ACK (throws BleLinkSendError)
client.disconnect()
```

A delegate (`BleLinkClientDelegate.bleLinkClient(_:didEmit:)`) receives the same events, if you prefer that to
`AsyncStream`. `send(_:completion:)` is the callback variant of `send`.

API summary: `connect(identifier:timeout:)` (HELLO included) -> server `HelloBody`; `send(_:) -> msgId`;
`sendAndWait(_:)`; `cancel(_:)`; `resetSession()` (re-HELLO, for example after the MTU grew); `disconnect()`.
Events: `received`, `sent`, `failed`, `progress`, `peerReady`, `disconnected`, `helloTimeout`.
Failure reasons: `disconnected, reset, timeout, crc, tooLarge, busy, rejected(status), linkUnreliable, cancelled`.

Limits that this client advertises in its HELLO are in `BleLinkConfig` (default: maxPacket 512, maxMessage 16 MiB,
window 4, PROGRESS wanted, timeout 10 s). Incoming messages are reassembled in memory, so `maxMessage` bounds the
RAM one message can take (times `window`).

## Threading (main-actor safety)

- All CoreBluetooth callbacks and all protocol state live on one private serial queue, never the main thread,
  so a busy UI cannot stall the radio and notifications are taken off the delegate queue at once (14.2).
- Every public method is thread-safe (it hops onto that queue) and can be called from any actor.
- Events go to `delegateQueue` (default `.main`): with the default, the delegate and the `events` loop may update
  UI directly. `BleLinkClient` itself is not `@MainActor`. Pass another queue to keep event handling off main.
- `events` is a single-consumer `AsyncStream` with an unbounded buffer: iterate it from one task, and keep up
  (each `received` event holds the whole message).
- `BleLinkSession` (core) is not thread-safe; it is driven from one serial context, which `BleLinkClient` provides.

## Transport details

- Discovers service `7B1E1000-...`, characteristics RX (`...1001`) and TX (`...1002`), enables TX notifications,
  then writes HELLO with response (if RX allows it); all other packets go out as Write Without Response.
- `osLimit` = `peripheral.maximumWriteValueLength(for: .withoutResponse)`, re-read before every write burst,
  so `packetSize = clamp(min(server maxPacket, osLimit), 20, 512)` (section 5). iOS cannot request an MTU.
- Flow control: a packet is pulled from the session only while `canSendWriteWithoutResponse` is true; otherwise
  the client waits for `peripheralIsReady(toSendWriteWithoutResponse:)`. Writes issued while it is false would be
  dropped silently by iOS. ACK timers are paused while flow control blocks longer than T/2 (section 10).
- A 0.25 s timer drives HELLO resend, ACK timeout and the receiver idle timeout.
- `peripheral(_:didModifyServices:)` for the BleLink service (server app restarted) -> rediscover and HELLO again.
- The INFO characteristic is not read; HELLO carries the same data.

## App setup (Info.plist, background)

- `NSBluetoothAlwaysUsageDescription` (iOS 13+) is **required**; without it the app is killed on first Bluetooth
  use. Apps that still deploy to iOS 12 also need `NSBluetoothPeripheralUsageDescription`. On macOS, a sandboxed app
  needs the "Bluetooth" capability (`com.apple.security.device.bluetooth`) as well.
- Background: add `bluetooth-central` to `UIBackgroundModes` ("Uses Bluetooth LE accessories") to keep the link while
  the app is in the background. With it, iOS wakes the app for TX notifications and writes continue, but at
  lower priority and without guarantees on time; scanning in the background needs explicit service UUIDs (this
  client scans for the BleLink service) and is much slower. Without the mode, iOS suspends the app within seconds
  and the connection is torn down shortly after.
- **Suspended mid-transfer**: while the app is suspended, no code runs. Outgoing messages stop at whatever packet
  was last written; incoming notifications are queued by the OS for a short while (with `bluetooth-central`) or
  the link is dropped (without it). If the link drops, every in-flight message fails with `.disconnected`, the
  server discards its partial copies, and nothing is resumed across connections (v1, section 11). A message that
  was fully sent but not yet ACKed is *in doubt*: the server may have delivered it. After reconnect, resend
  idempotent messages or de-duplicate with an application-level id. If the app is resumed before the server's
  timeouts fire, the transfer simply continues (timers measure idle time, section 10).
- App killed / force-quit: the connection is dropped (same as a disconnect). State restoration: pass
  `restoreIdentifier:` to `BleLinkClient(...)`; on restoration the client rediscovers, re-subscribes and sends a new
  HELLO, which resets the server's session (all messages from before are failed with `reset` on both sides).
- For long transfers that must finish, keep the app in the foreground (or use `beginBackgroundTask` for the last
  seconds); BLE background time is not a transfer guarantee.

## Tests

```sh
cd clients/ios/BleLink
swift test        # codec + session against docs/protocol-test-vectors.json, no Bluetooth needed
swift build       # also type-checks the CoreBluetooth client and the SwiftUI sample on macOS
xcodebuild -scheme BleLink -destination 'generic/platform=iOS' build   # compile check for iOS
```

The vectors are read from `docs/protocol-test-vectors.json` in the repository (or `$BLELINK_VECTORS`), not copied.
CI runs `swift test` and the iOS compile check on `macos-latest` (job `ios-client`). What is not verified without
hardware is listed in `NOTES.md`.
