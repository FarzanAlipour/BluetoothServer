# blelink: BleLink v1 reference client (Python, bleak)

A small reference implementation of the **client (GATT central) side** of `docs/PROTOCOL.md`. It is:

- the hardware test tool (`blelink-client`, driven by `scripts/run_hw_test.sh client ...`), and
- the example other client developers port from (Android, MCU, desktop). Every protocol rule has a
  `PROTOCOL.md` section reference next to it in the code.

It is written from the spec only. It does not import or mirror the C++ server code. The conformance tests run it
against `docs/protocol-test-vectors.json` and against itself in the server role.

## Install

```sh
python3 -m venv .venv && . .venv/bin/activate
pip install -e "clients/python[test]"        # bleak is pinned (see pyproject.toml)
```

Python 3.10 or later. The codec, session and loopback modules need only the standard library; `bleak` is imported
only when you connect to real hardware.

## Layers

| Module | What it does | I/O |
|--------|--------------|-----|
| `blelink.codec` | packet encode/decode, CRC-32, UUIDs and constants (sections 1, 2, 4.1) | none |
| `blelink.session` | sans-IO state machine: HELLO, packetSize per direction, framing, window, ACK, progress, interleaving, seq-gap detection, RESUME / MSG_RESEND, CANCEL, timeouts, disconnect (sections 3-11, 16) | none, no clock |
| `blelink.link` | asyncio driver that runs a `Session` over a transport with flow control (6.2) | asyncio |
| `blelink.ble` | bleak transport: scan, connect, MTU, TX notify, RX writes, INFO read | bleak |
| `blelink.loopback` | in-memory transport and fake echo server (a server-role `Session`), for tests and `--loopback` | asyncio |
| `blelink.cli` | `blelink-client` | |

To port to another platform, copy `codec` and `session` (they need nothing but bytes and a monotonic time value)
and write a driver like `link.py` around your BLE stack.

### Library use

```python
from blelink import Link, Session
from blelink.ble import BleakTransport

t = await BleakTransport.connect(name="BLE-TestServer")
link = Link(t, Session("client", os_limit=await t.wait_for_mtu()))
ready = await link.open()                 # TX notify on, HELLO (with response), wait for server HELLO
print(ready.tx_packet_size, ready.peer)   # negotiated packetSize client->server, server limits
await link.send(b"hello")                 # resolves on ACK OK, raises SendError otherwise
msg = await link.receive(timeout=10)      # next complete, CRC-verified server message
await link.close()
```

## CLI

```
blelink-client [--name BLE-TestServer | --address ADDR] [--with-response] [-v|-vv] COMMAND ...

  info                           connect + handshake, print negotiated values
  send-text TEXT                 one UTF-8 message, wait for ACK
  send-blob SIZE                 SIZE bytes (512, 4k, 1M), throughput
  echo-test SIZE...              each size, wait for the identical echo (server in echo mode)
  interleave LARGE SMALL         two concurrent messages; small one should be ACKed first (window >= 2)
  soak N [--max-size S] [--verify-echo]   N random-size messages with the window kept full

  --loopback                     no hardware: in-memory echo server
```

Exit code: `0` PASS, `1` FAIL (a check failed), `2` no connection / handshake failed / bad usage. The last line is
always `RESULT: PASS ...` or `RESULT: FAIL ...` with a throughput summary. The output also shows the negotiated
ATT MTU, the OS write-without-response limit, the flow-control mode, both HELLOs, the packetSize per direction,
timeout `T`, and the session counters (seq gaps, RESUMEs, restarts, ACK-timeout resends, CRC resends,
duplicates, ignored packets).

Hardware runs need **two machines**: the server (`ble_test_server` or the Unity app) on one, this client on
another host that can act as a central. A phone cannot run this tool. See `NOTES.md` for what is still to verify
on hardware.

## Flow control per bleak backend (PROTOCOL.md 6.2)

- **WinRT** and **BlueZ**: `write_gatt_char(response=False)` is awaited; the await is the backpressure.
- **CoreBluetooth (macOS)**: bleak does not wait for `canSendWriteWithoutResponse`. `ble.py` polls it through
  bleak's private backend object. This is why bleak is pinned. If the attribute is missing, writes are not
  throttled; CoreBluetooth may then drop packets, and RESUME repairs the gaps (slower, but still correct).
  `--with-response` is the lossless fallback.
- HELLO is always sent with response (section 1). Data uses write-without-response unless `--with-response`.

## Tests

```sh
cd clients/python && python -m pytest -q
```

- `tests/test_vectors.py`: CRC-32, every packet vector (decode and re-encode), message splits at packetSize
  20/182/244/509 (byte exact), and the receiver reassembling them.
- `tests/test_session.py`: handshake, packetSize derivation and clamps, timeouts, window, interleaving, early
  ACK, CRC retry, seq wrap, lost data / MSG_START / ACK / RESUME, restart limits, CANCEL, reset, disconnect,
  msgId wrap. Uses a fake clock, so no real waits.
- `tests/test_link_cli.py`: the asyncio `Link` and every CLI command over the loopback server, including dropped
  packets and a disconnect during a transfer.

CI runs these on every push (job `python-client` in `.github/workflows/ci.yml`). No hardware is involved.
