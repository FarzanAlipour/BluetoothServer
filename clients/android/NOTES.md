# Android BleLink client: implementation notes

Implemented from `docs/PROTOCOL.md` (BleLink v1, commit 5991291) and `docs/protocol-test-vectors.json` only; no
server code was ported.

## Verified (no hardware)

Run on macOS with JDK 21, AGP 8.7.2, Kotlin 2.0.21, Gradle 8.14.3, compileSdk 35:

- `./gradlew test`: 47 JVM unit tests pass (debug and release variants).
  - `CodecVectorTest` (5): every CRC32 case; every single packet in `packets` decoded to its fields and re-encoded
    byte for byte; INFO; forward compatibility (unknown type, short packets, trailing bytes).
  - `SessionVectorTest` (8): client HELLO bytes; the 1000-byte message splits at packetSize 20/182/244/509 as
    **sender** (exact bytes) and **receiver**; the packetSize-20 boundary cases both ways; interleaving
    (completion-order delivery and both ACKs); resume as receiver (exactly one RESUME(20, 24), then repair) and as
    sender (MSG_RESEND + MSG_DATA, see S1).
  - `SessionBehaviourTest` (31): handshake and HELLO resend/give-up, effective timeout = max, window and maxMessage
    both ways, every ACK status, CRC auto-resend limit, ACK timeout (3 resends then `timeout` + CANCEL), RESUME
    de-duplication and the 8-restart limit, CANCEL, PROGRESS both ways, peer-initiated reset answered once and no HELLO ping-pong on a late reply, disconnect,
    duplicate MSG_START after a lost ACK, overlong payload, unknown msgId -> one RESUME(id, 0), MSG_RESEND with a
    wrong offset, seq wrap, idle timeout (3 RESUMEs then discard).
  - `BleLinkClientLoopbackTest` (3): `BleLinkClient` over a fake transport whose far end is a second session playing
    the server: both directions at once (50 KB up, 3 KB down, packetSize 182) with never more than one write
    outstanding; a dropped client packet repaired through RESUME/MSG_RESEND; disconnect fails unfinished sends.
- `./gradlew lint`: 0 errors (library: no issues; sample: 3 cosmetic warnings, no icon / targetSdk / backup rules).
  This includes the NewApi checks for every API 26/31/33 call against minSdk 23.
- `./gradlew :sample:assembleDebug` builds the sample APK. The README usage example is compiled as
  `sample/.../ReadmeExample.kt`.

## Needs a hardware run against `ble_test_server` (NOT verified)

Nothing in `GattTransport` or the sample has run on a phone. Precondition: `ble_test_server` must serve the BleLink
v1 service (the server-side protocol engine task); on the base branch it serves only the GATT test profile.
Covered by the manual task t_2890c7eb. To check:

1. Scan with the service UUID filter, connect, discover, `requestMtu(517)`, CCCD write, HELLO with response, server
   HELLO: on Android 12+ (runtime BLUETOOTH_SCAN/CONNECT) and on Android 6-11 (location permission, Location on).
2. Negotiated MTU per phone (expect 517 -> packetSize 512 up to the server's maxPacket; some phones give 247 or less)
   and the resulting `sendPacketSize`.
3. Flow control: Write Without Response paced by `onCharacteristicWrite` during a 1-16 MiB upload with no seq
   gaps on the server (a gap shows up as RESUME traffic). How often `writeCharacteristic` is refused as busy and
   whether the 10 ms x 50 retry is ever exhausted.
4. Throughput at packetSize 182/244/512 with connection priority HIGH, and with/without LE 2M PHY
   (`onPhyUpdate` is not observed; whether the PHY changed is not reported).
5. Server -> phone notification bursts (1 MB down): no lost notifications, PROGRESS from the phone after each 64 KiB.
6. Interleaved small + large messages both ways, completion-order delivery.
7. Disconnect mid-transfer (walk away, Bluetooth off, server killed): `Failed(DISCONNECTED)` for every unfinished
   send, then `Disconnected`; reconnect via `connect()` on the same client; status 133 handling.
8. API 33+ write/notify signatures vs. the deprecated ones on API < 33: needs one phone on each side of API 33.
9. `onMtuChanged` after setup (renegotiation) and the 10 s per-GATT-operation timeout firing on a stuck stack.
10. Background / screen off: not handled by the library (needs a foreground service in the app), not tested.
11. Stale GATT cache after the server app changes its services: no `refresh()` workaround is implemented; if
    discovery finds no BleLink service on a phone that worked before, toggle Bluetooth or unpair.

## Spec issues (S1-S9 numbered as in clients/ios/BleLink/NOTES.md; reported on the spec task t_4ac5cc8f)

- **S1 - resume vector seq.** `resume.senderResendPacketsHex` uses seq 7, 8, 9, but after `originalPacketsHex`
  (seq 1..5) the sender's next seq is 6. This client sends 6, 7, 8. `senderAnswersResumeWithMsgResend` checks seq
  6.. and compares the vector bytes with the seq byte rewritten; `receiverRepairsWithVectorBytesAsIs` feeds the
  vector unchanged and expects the extra gap to cause a second RESUME(20, 24) before the ACK (harmless).
- **S2 - repeated gaps while in resync.** Android follows 8.1 **literally**: on every seq gap it sends RESUME for
  every incomplete message, including messages already waiting for MSG_RESEND (the sender ignores the duplicate
  per 8.2). **The iOS client skips messages already in resync**, so the two clients differ. The spec should say which.
- **S3 - reset ping-pong.** As iOS: `resetSession()` goes to OPENING and treats the next HELLO as the answer; a
  server-initiated HELLO while READY is answered once and the session stays READY. The spec should say that the
  initiator leaves READY until it gets the answer.
- **S4 - duplicate memory vs. CRC retry.** As iOS: only status 0 is remembered as "completed" (last `window` ids);
  status 1 is not remembered, so the sender's same-msgId CRC retry is accepted. Early rejections 2/3 are kept in a
  separate 16-entry list so their trailing MSG_DATA is ignored silently. The client never sends status 4 (there is
  no application-reject hook). The spec should say which statuses go into duplicate memory.
- **S5 - window 0** from the peer is treated as 1.
- **S6 - version 0 HELLO.** Android accepts any HELLO version and runs v1. **iOS ignores version 0.** Spec says
  "not possible today"; it should say what a v1 receiver does with 0.
- **S7 - CANCEL after giving up.** As iOS: after 3 ACK-timeout resends (`timeout`) or 8 restarts (`link_unreliable`)
  the client sends CANCEL reason 1.
- **S8 - PROGRESS flag.** Android sends PROGRESS for an incoming message whenever the **server's** HELLO has flag
  bit 0 (literal 16: "when flags bit 0 was set in the sender's HELLO"), regardless of our own `wantProgress`.
  **iOS requires both.** Minor; noted so the server can rely on one reading.
- **S9 - HELLO write error.** A failed HELLO write is not acted on; the HELLO timeout (3 resends, then disconnect)
  covers it.

## Design choices (not from the spec)

- Control packets (HELLO, ACK, RESUME, CANCEL, PROGRESS) are written before data; data is round-robin over in-flight
  messages, one packet each. Seq numbers are assigned when a packet is polled, so the transport must write (or
  report as failed) every packet it is given. A packet the stack refuses is lost on purpose; the server repairs it
  with RESUME like any other gap.
- HELLO advertises `maxPacket = osLimit` (mtu - 3) unless `LocalLimits.maxPacket` is set. A later MTU change only
  changes our send size; call `resetSession()` to advertise the new value.
- Incoming messages are buffered whole (allocated at MSG_START), so memory is bounded by `maxMessage * window`
  (defaults 16 MiB x 4). There is no streaming-receive API.
- `Progress(SENT_TO_STACK)` events are local (bytes handed to the stack, ~1 % steps); `PEER_CONFIRMED` comes from
  the server's PROGRESS packets; `RECEIVING` is our own receive progress (provisional until `Received`).
- Messages beyond the server's `window` wait in an unbounded local queue; the app bounds it.
- The session runs under one lock in `BleLinkClient`; timers tick every 250 ms on the client's coroutine scope.
