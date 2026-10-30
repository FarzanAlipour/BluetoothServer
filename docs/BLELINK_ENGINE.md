# BleLink engine (src/common) - implementation notes

The BleLink v1 protocol (docs/PROTOCOL.md) is implemented as a pure C++17 engine in `src/common`:

| File | Content |
|------|---------|
| `BleLinkCodec.h/.cpp` | Packet codec (all 8 packet types, INFO value), CRC-32/IEEE, `SplitMessage` reference framing |
| `BleLinkEngine.h/.cpp` | `BleLink::Engine`: sessions per peer, HELLO negotiation, sender, receiver, timers |
| `tests/test_blelink.cpp` | ctest `blelink`: codec vectors, scripted peer, simulated link, threaded smoke test |

No OS headers (`scripts/check_layers.sh`). The engine is built into the BleServer library but nothing
calls it yet: the C ABI / Unity wrapper and the backend wiring are follow-up tasks.

## Backend interface

```
ITransport::SendPacket(peer, bytes, len) -> Sent | Busy | Error     // one notification / one write
ITransport::RequestDisconnect(peer)                                  // HELLO handshake failed
Engine::OnPeerConnected / OnPeerSubscribed / OnPeerUnsubscribed / OnPeerDisconnected (peer)
Engine::OnPacket(peer, bytes, len)                                   // RX write / TX notification received
Engine::OnTransportReady(peer)                                       // "ready again" after Busy
Engine::SetPeerOsLimit(peer, ATT_MTU - 3)                            // optional, default 20
```

Events out (handler or `PollEvent`): `PeerReady(info: txPacketSize, peer limits, T)`, `PeerGone(reason)`,
`MessageReceived(peer, msgId, data)`, `MessageSent(handle, msgId)`, `MessageFailed(handle, reason, ackStatus)`,
`Progress(Sending | PeerReceived | Receiving, bytes, total)`. `Send()` returns a handle (engine-wide, never 0).

Threading: every input only appends to an inbox under a short mutex (safe from a BLE callback thread and from
inside `SendPacket`). All protocol work happens in `Pump()`, driven by the engine's worker (`Start()`) or by the
caller. `ITransport` is called from `Pump()` with the core lock held; events are handed out after the lock is
released, so handlers may call back into the engine. `Send()` computes the CRC on the caller's thread.

Sending is peek/commit: a packet is built from the current state and the state (seq, offsets, queues) advances only
when `SendPacket` returned `Sent`. Busy waits for `OnTransportReady` (or `busyRetryMs`, a lost-wakeup guard).
Error retries after `errorBackoffMs`; after `maxSendErrorsPerPacket` the packet is given up with its seq consumed,
so the peer sees a seq gap and RESUME / ACK timeout repair it.

Scheduling per peer: HELLO first, then control packets (ACK, RESUME, CANCEL, PROGRESS) FIFO, then data
round-robin over the in-flight messages (one packet each). New messages start while
`inFlight < window_peer`; msgIds count up from 0 per session.

## Interpretations and disagreements with the spec

The spec wins; these are the places where it was silent or where the engine picks one reading.

1. **HELLO ping-pong.** Section 3: a HELLO while READY resets the session and the non-initiator answers. A stack
   duplicate of a HELLO (or of the answer) then makes both sides reset and answer each other forever. Engine rule:
   the server always resets and answers (that is what makes a client retry after a lost answer work); the
   client drops a server HELLO that is byte-identical to the previous packet with the previous seq (nothing in
   between). That terminates every ping-pong after at most one extra round. Suggest adding this to the spec.
2. **Stack duplicates.** Not in the spec. A packet identical to the previous one with seq = expected - 1 is
   dropped (`Stats::duplicatesDropped`); without this a duplicate looks like a 255-packet gap. Non-adjacent
   duplicates go through the normal gap/RESUME path (tested).
3. **Duplicate memory.** Spec: the last `window` completed ids. The engine keeps `window + 16` entries because
   early rejections (ACK 2/3) are remembered too, so that their trailing MSG_DATA is ignored silently (6.3).
   Ids that failed with ACK 1 (CRC) are **not** remembered, or the sender's automatic resend (same id) would be
   answered with the stored ACK 1.
4. **Restart limit vs. D2.** On a gap every incomplete message is resynced, so with interleaving a gap in one
   message also uses up restarts of the others. On a lossy link the 8-restart limit (8.2) therefore fails
   messages sooner than per-message loss would suggest. Fine for BLE (loss is rare); worth a note in the spec.
5. **Resume test vector.** `resume.senderResendPacketsHex` starts at seq 7, while the original stream ended at
   seq 5 (diagram 15.4 uses seq 6). A receiver therefore also sees a gap 5 -> 7 and sends a second
   `RESUME(20, 24)`, which the sender ignores. Harmless; the test accepts it. Consider regenerating the vector
   with seq 6.
6. **Timer pause under flow control (section 10).** ACK timers do not fire while the transport is Busy. When it
   becomes ready after more than T/2, the blocked time is added to the timers of messages waiting for an ACK.
7. **Reset and queued messages.** On a reset (either side) in-flight messages fail with `reset`; messages queued
   but not started stay queued and start after the new HELLO. The msgId counter restarts at 0 (the peer cleared
   its duplicate memory). `Send()` during a reset is accepted (it is rejected only if the session never was READY).
8. **HELLO before CCCD.** A server accepts a client HELLO while LINKED and sends its answer as soon as the peer
   subscribes (section 3 says "ignore everything but HELLO" without saying when to answer).
9. **Receiver leniency.** Packets larger than our `maxPacket` are accepted. Bounds that are enforced:
   `maxMessage`, `window`, bytes > `totalLen` (-> ACK 1).
10. **Extra failure reasons.** Besides the spec's list the engine reports `cancelled` (app cancel), `not_ready`
    (Send to a peer that is not READY) and `shutdown`.
11. **PROGRESS.** Sent each 64 KiB when the peer's HELLO has flags bit 0. Local `Progress` events are raised every
    64 KiB for sending, peer-reported and receiving progress (messages of at least 64 KiB only).

## Tests (ctest `blelink`, ~5 s; ~1 min under ASan)

- Codec: every packet, CRC case, INFO value, 1000-byte split at 20/182/244/509 and the 20-byte boundary cases from
  `docs/protocol-test-vectors.json`; decode edge cases (empty, unknown type, short, trailing bytes).
- Scripted peer: handshake (limits, packet size, T, pre-HELLO packets ignored), interleaving vector (completion
  order 11, 10 and exact ACK bytes), resume vector (exact RESUME bytes, receiver repair, sender MSG_RESEND
  payloads), receiver limits (too large, window, CRC, duplicate START re-ACK, overflow, unknown id RESUME once,
  CANCEL, unknown type, 0/1-byte), gap + wrong-offset RESEND + idle timeout give-up, sender (ACK statuses, CRC
  retries, ACK timeout + CANCEL, 8 restarts, duplicate RESUME guard, window 1, oversize, cancel, transport errors),
  reset, client HELLO retry + handshake failure, client duplicate HELLO.
- Simulated link (two or more engines, fake clock): packet sizes 20/182/244/509 (both directions, at once, sizes 0,
  1, exactly one packet, boundaries, 100 KB), asymmetric 182/244, busy back-pressure, fixed and random seq gaps,
  back-to-back and late duplicates, disconnect mid-message + reconnect, interleaved small + 2 MB, window full,
  CRC mismatch (one-off and persistent), oversize, lost ACKs, 3 peers with different MTUs and limits, reset,
  16 MiB both directions.
- Threaded: worker threads, real clock, concurrent link thread and Send() from two threads.

## Still needs a manual hardware check

Nothing here has touched a radio. To verify once the backends feed the engine:

- Busy/ready mapping per server backend: macOS `updateValue` false -> `peripheralManagerIsReady`; Windows
  `NotifyValueAsync` completion; BlueZ notify fd `EAGAIN`/`POLLOUT`. A wrong mapping shows up as seq gaps
  (`Stats::seqGaps`, `resumesSent`) on the client.
- OS limit per peer: `CBCentral.maximumUpdateValueLength`, `GattSession.MaxPduSize - 3`, BlueZ `MTU`, and an MTU
  change mid-session (packet size follows `SetPeerOsLimit`; no reset is sent).
- Real clients at 20 (no MTU exchange), 182 (iOS), 244 (MCU / DLE) and 509/512 (Android 517): throughput and that
  `txPacketSize` matches what the client sees.
- iOS writes silently dropped when `canSendWriteWithoutResponse` is false: the receiver must recover by RESUME.
- Multiple centrals connected at once (one session each) on each desktop OS.
- Disconnect/reconnect and app backgrounding on phones: in-flight messages fail with `disconnected`, the
  re-sent message arrives once.
- Timeouts with a slow MCU advertising a large `timeout` (flash writes).
