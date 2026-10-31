# blelink (Python) - implementation notes

## Status

- Codec and session are implemented from `docs/PROTOCOL.md` only. They pass every vector in
  `docs/protocol-test-vectors.json`: CRC-32, all 12 packet vectors plus INFO, message splits at packetSize
  20/182/244/509, the boundary cases at 20, interleaving, and RESUME, on both the sender and the receiver side.
- The session's server role exists only for tests and `--loopback`. The real server is the C++ library.
- bleak is pinned to `3.0.2`. `ble.py` reads `client._backend._peripheral.canSendWriteWithoutResponse` on macOS,
  which is private API. Re-check it, and re-run the hardware list below, before you change the pin.

## Spec vs. task

The task card and `PROTOCOL.md` agree. Two additions go beyond the card:

- The card lists the commands `send-text`, `send-blob`, `echo-test` and `soak`. The CLI adds `info` (handshake
  only) and `interleave` (spec section 15.3), because the card asks for interleaving to be reported.
- Section 1 says HELLO SHOULD use Write With Response. The client always does this, even when data uses Write
  Without Response.

## Dependencies for hardware runs

- **There is no BleLink server yet.** `ble_test_server` currently exposes the old GATT test profile
  (write / echo / large characteristics), not the BleLink service `7b1e1000-...`. Hardware runs of this client
  need the C++ BleLink server plus an echo mode: every delivered message is sent back as a new message. That
  echo mode is needed by `echo-test`, `interleave --verify-echo` and `soak --verify-echo`. `send-text`,
  `send-blob` and `soak` without `--verify-echo` only need ACKs.
- You need two machines: the server runs on one, this client on another host that can act as central
  (Linux/BlueZ, Windows, or macOS). A phone cannot run it.

## Still to check manually on hardware

1. **macOS / CoreBluetooth flow control.** Confirm that `canSendWriteWithoutResponse` polling works with
   bleak 3.0.2: `connected: ... flow control: CoreBluetooth canSendWriteWithoutResponse`, and `send-blob 1M`
   reports `seq gaps 0` on the server. Without it, expect gaps that RESUME repairs.
2. **BlueZ (Linux).** Check that `max_write_without_response_size` reports ATT_MTU-3 after the MTU exchange
   (BlueZ older than 5.62 always reports 20; `--mtu-wait`). Check that awaited `WriteValue` "command" calls keep
   the queue bounded: no seq gaps on `send-blob 4M`.
3. **Windows (WinRT).** Check MTU / `max_write_without_response_size`, and that awaiting
   `WriteValueWithResultAsync` is enough backpressure.
4. **packetSize per direction.** With the server at maxPacket 512 and a client MTU of 247, the client should
   send 244-byte packets. Check that server notifications never exceed the client's ATT_MTU-3 (sections 5, 14).
5. **Interleaving on air.** `interleave 256k 16`: the small message is ACKed before the large one when the
   server window is 2 or more.
6. **RESUME on real loss.** Force drops (BlueZ `btmgmt` with a small connection interval, or pull the
   server-side queue limit down), then check that the counters show seq gaps / RESUME / restarts and the
   result is still PASS.
7. **Timeouts.** Stop the server process in the middle of `send-blob 1M`. The client should report
   `disconnected` (supervision timeout) or `timeout` after at most 4 T, then exit 1.
8. **Disconnect and reconnect.** Turn Bluetooth off on the server during `soak 500`. The client must exit
   1 with `disconnected` and must not hang. Run again: msgIds restart from 0 (section 11).
9. **Throughput baseline.** Record the `send-blob 1M` and `echo-test 1M` numbers per OS pair in
   `docs/TESTING.md` (results table).
10. **INFO characteristic.** It should be readable before the session starts, with the same values as the
    server HELLO.
11. **`--with-response`.** It should be lossless and slower. Use it as a sanity reference for item 1.

## Known limitations

- One connection per process. No automatic reconnect: the CLI reports the failure and exits. A caller that
  wants retries opens a new `Link` (section 11).
- Streaming receive (section 9) is not implemented. The client reassembles each message in memory, which is
  fine for a desktop central. It advertises `flags = PROGRESS` and never `STREAMING`.
- The loopback server's echo is immediate. Real servers add latency, so loopback throughput numbers do not
  predict air throughput.
