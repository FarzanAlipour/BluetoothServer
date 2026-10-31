# iOS BleLink client: implementation notes

Implemented from `docs/PROTOCOL.md` (BleLink v1, commit 5991291) only; no server code was ported.

## Verified (no hardware)

- `swift test` (macOS, Swift 6.2 toolchain, tools-version 5.9): 42 XCTests pass.
  - `VectorTests` (11): every case in `docs/protocol-test-vectors.json`: CRC32, all single packets decoded and
    re-encoded byte for byte, client HELLO, INFO, the 1000-byte message splits at packetSize 20/182/244/509 as
    **sender** (exact bytes) and **receiver**, the 20-byte boundary cases, interleaving (completion-order delivery
    and the two ACKs), resume as receiver (exactly one RESUME(20, 24)) and as sender (MSG_RESEND + MSG_DATA).
  - `SessionTests` (31): handshake, HELLO timeout/resend, effective timeout `max`, window and maxMessage on both
    sides, ACK statuses 1-255, CRC resend limit, ACK timeout (3 resends, then `timeout` + CANCEL), timer pause
    under flow control, RESUME de-duplication and the 8-restart limit, CANCEL, PROGRESS both ways, peer and
    client reset, disconnect, duplicate MSG_START after completion, overlong payload, unknown msgId -> one
    RESUME(id, 0), MSG_RESEND with a wrong offset, unknown types / short packets / trailing bytes, seq wrap,
    idle timeout (3 RESUMEs then discard), and the sender reproducing the interleaving vector.
- `swift build` on macOS (client + SwiftUI sample) and `xcodebuild -scheme BleLink -destination
  'generic/platform=iOS' build` succeed. The README usage example was compiled against the package.

## Needs a hardware run against `ble_test_server` (NOT verified)

Nothing in `BleLinkClient` (the CoreBluetooth adapter) has run against a real radio. Specifically:

1. Scan / connect / discover / `setNotifyValue` / HELLO with response against `ble_test_server` speaking BleLink v1.
   **Precondition**: the server side of BleLink v1 (task "BleLink protocol engine in src/common") must be merged
   into `ble_test_server`; on the base branch it serves only the GATT test profile, not this service.
2. `maximumWriteValueLength(for: .withoutResponse)` values on real iPhones (expect 182 or 244/509) and the
   resulting packetSize; whether it changes after connection (we re-read it before every write burst).
3. `canSendWriteWithoutResponse` / `peripheralIsReady(toSendWriteWithoutResponse:)` flow control under a 1-16 MiB
   transfer: no seq gaps on the server (a gap would show as RESUME traffic).
4. Throughput numbers (the sample logs KB/s per message) at 182 / 244 / 509 byte packets.
5. Server -> client messages: notification burst handling, PROGRESS from the client for messages > 64 KiB.
6. Disconnect mid-transfer (walk away / toggle Bluetooth): all in-flight messages fail `disconnected`.
7. Background with `bluetooth-central`, app suspended mid-transfer, and state restoration (`restoreIdentifier`):
   documented in README from Apple's documented behaviour, not tested.
8. `didModifyServices` when the Unity/server app restarts while the phone stays connected.
9. macOS central (running the sample as a macOS app against the server on another machine).
10. `disconnect()` during a pending connect: we drop the peripheral reference ourselves because it is not certain
    that `didDisconnectPeripheral` fires for a cancelled pending connection.

## Spec issues found (reported on the spec task t_4ac5cc8f)

- **S1 - resume vector seq.** `resume.senderResendPacketsHex` uses seq 7, 8, 9, but the sender's last packet in
  `originalPacketsHex` had seq 5 and nothing else was sent, so its next seq is 6. Either the vector should use 6, 7,
  8 or the description should say a packet with seq 6 was sent (and lost). This client sends 6, 7, 8; the
  test compares everything except the seq byte, and the receiver test accepts the vector as-is (the extra gap
  is harmless, see S2).
- **S2 - repeated gaps while in resync.** 8.1 says "for every message currently being received" send RESUME on a gap.
  Read literally, a second gap while a message already waits for its MSG_RESEND sends another RESUME with the
  same offset. This client skips messages that are already in resync (the sender would ignore the duplicate
  anyway under 8.2). The spec should state which is intended.
- **S3 - reset ping-pong.** Section 3: "The side that did not initiate the reset answers with its own HELLO". The
  spec does not say that the *initiator* leaves READY. If an initiator stays READY, the answer HELLO looks like a
  new reset, which it answers again: an endless loop. This client goes to OPENING when it initiates (so it does not
  answer the reply) and answers a server-initiated HELLO once while staying READY. The server must go to OPENING
  when it initiates, or the spec should add "a HELLO received while waiting for the answer to our own HELLO is
  the answer". Should be pinned down in the spec.
- **S4 - duplicate memory vs. CRC retry.** 6.3 says the receiver remembers the last `window` completed msgIds
  "with their statuses" and re-ACKs a MSG_START for them with the stored status. But after ACK status 1 (CRC) the
  sender resends with the **same** msgId (8.3 / 6.3 table); if status 1 were remembered, every resend would be
  answered with the stored 1 and never delivered. This client remembers only status 0 as "completed"; status 1 is
  not remembered. Statuses 2/3/4 (early rejections) are remembered separately (16 entries) so that following
  MSG_DATA is ignored silently as 6.3 requires. The spec should say which statuses go into duplicate memory.
- **S5 - window 0.** 4.1 says `window` MUST be >= 1 but not what a receiver of `window = 0` does. Treated as 1.
- **S6 - version 0 HELLO.** Section 12 says it is "not possible today". This client ignores a HELLO with version 0
  (stays OPENING; the HELLO timer eventually gives up).
- **S7 - CANCEL after giving up.** 10 says the message "fails with `timeout`" after 3 ACK-timeout resends; it does not
  say whether CANCEL is sent. CANCEL reason 1 ("sender gave up") exists, so this client sends it, letting the
  receiver free its state at once instead of waiting for its idle timeout.
- **S8 - PROGRESS flag symmetry.** Flag bit 0 means "wants PROGRESS and can send it". This client sends PROGRESS only
  when the server set bit 0 **and** its own config has it set. The spec is clear enough; noted for consistency.
- **S9 - HELLO write error.** 1 says HELLO SHOULD use Write With Response "so the client learns that it was
  accepted", but not what to do on an ATT error. This client relies on the HELLO timeout (resend 3x, then give up).

## Design choices (not from the spec)

- Control packets (HELLO, ACK, RESUME, CANCEL, PROGRESS) are written before data packets; data is round-robin
  across in-flight messages, one packet each. Seq numbers are assigned when a packet is pulled, so the transport
  must write every packet it pulls.
- Outgoing progress events are local (bytes handed to CoreBluetooth, every ~1 %); `confirmedByPeer` progress
  comes from the server's PROGRESS packets.
- Messages beyond the server's `window` wait in a local queue (no limit; the app bounds it).
- The "unknown msgId -> RESUME once" memory is cleared after T, so another RESUME(id, 0) can follow (8.1:
  "until ... the idle timeout fires").
