# Testing

Three levels, from fastest to most realistic. Each one finds bugs the previous one cannot.

| Level | What | Needs | Runs in CI |
|---|---|---|---|
| (a) Unit + fake-backend tests | `ctest`: chunker, C ABI layer, the GATT scenario against an in-memory backend | a compiler | yes (Linux, macOS, Windows, ASan/UBSan) |
| (b) CLI hardware test | `ble_test_server` on a real adapter, checked from nRF Connect | a computer with BLE peripheral support + a second BLE device | no, manual |
| (c) Unity sample | the same GATT profile through the Unity plugin: [samples/UnityBleSample](../samples/UnityBleSample/README.md) | Unity 2022.3.62f3 | no, manual |

Run (b) before (c): it has no Unity and no C# wrapper, so a failure there is a native bug.
The GATT layout used by (a), (b) and (c) is in [GATT_TEST_PROFILE.md](GATT_TEST_PROFILE.md).

> The `samples/UnityBleSample` link resolves once PR #5 (`feature/unity-sample-gatt-nrfconnect`) is merged.

## (a) Unit and fake-backend tests

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

| ctest name | File | Covers |
|---|---|---|
| `chunker` | `tests/test_chunker.cpp` | chunk format, reassembly |
| `abi` | `tests/test_abi.cpp` | C ABI layer: lifecycle, validation, callbacks, shutdown |
| `chunker_abi` | `tests/test_chunker_abi.cpp` | chunker through the C ABI: ACKs, retransmission, bounds |
| `scenario_gatt` | `tests/test_scenario_gatt.cpp` | the GATT test profile and the CLI's BleLink commands, end to end against the fake backend |
| `blelink` | `tests/test_blelink.cpp` | BleLink protocol engine: vectors, scripted peer, simulated lossy link |
| `blelink_abi` | `tests/test_blelink_abi.cpp` | `BleLink_*` C ABI on the fake backend |
| `notify_queue`, `peer_notify` | `tests/test_notify_queue.cpp`, `tests/test_peer_notify.cpp` | notification flow control, per-central notify |

`scenario_gatt` compiles the **same server code as the CLI** (`examples/ble_test_server/TestServerApp.cpp`) and plays the
central through the fake backend's `Simulate*` hooks: init, add profile, advertise, connect, read (value format),
write (hex + UTF-8 logged), subscribe/heartbeat notifies, echo round trip, disconnect, reconnect, stop advertising,
shutdown. For BleLink it runs a client-role BleLink engine as the central (`m`, `b 1M`, `p`, received-message logging,
`--echo-messages` round trips of 0 B to 200 KB) and writes the **hand-typed nRF Connect hex below byte for byte**,
checking every notification the server sends back. Sanitizer run (macOS/Linux):

```bash
cmake -S . -B build-san -DENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-san
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-san --output-on-failure
```

Not covered by (a): real advertising, real connections, MTU limits, OS permissions, pairing, timing.

## (b) CLI hardware test with nRF Connect

### Start the server

```bash
scripts/run_hw_test.sh            # macOS / Linux
```
```powershell
.\scripts\run_hw_test.ps1         # Windows (powershell -ExecutionPolicy Bypass -File ... if blocked)
```

Both build Release into `build-hw/`, start `ble_test_server --verbose` and write the log to
`logs/hw-test-<os>-<timestamp>.log` (git-ignored). **Attach that log to bug reports.** Extra arguments are passed on:
`--name NAME` (default `BLE-TestServer`), `--heartbeat-ms N` (default 1000), `--run-seconds N`, and
`--echo-messages` (BleLink: every received message is sent back to its sender, for round-trip tests).
Manual build: `cmake -S . -B build && cmake --build build --target ble_test_server`, binary in `build/examples/`.

Commands on stdin:

| Command | Effect |
|---|---|
| `m <text>` | BleLink: send `<text>` as one message to every READY peer (`m` alone: an empty message) |
| `b <size>` | BleLink: send a generated blob to every READY peer: `b 1000`, `b 64k`, `b 1M` (k = 1024) |
| `p` | list BleLink peers: state, packet size, ATT MTU, messages in flight / queued / receiving, peer limits |
| `n` | notify now on the Notify characteristic |
| `s` | stop/start advertising |
| `h`, `q` | help, quit (Ctrl-C also shuts down cleanly) |

Wait for `Advertising as "BLE-TestServer"`; every event is logged with a timestamp. Tags: `[info]` our own
progress, `[backend]` information the platform backend reports (state changes, diagnostics), `[link]` BleLink
sessions, `[send]` / `[recv]` / `[echo]` / `[progress]` BleLink messages, `[peers]` the `p` listing.
Only real failures are `[error]` lines; they carry the `BleError` code and name, e.g. `BluetoothDisabled (7)`, or the
BleLink failure reason (`timeout`, `disconnected`, ...).

### Per-OS setup

**macOS (CoreBluetooth)**
1. Xcode command line tools (`xcode-select --install`) and CMake.
2. Bluetooth permission belongs to the app that runs the program: **Terminal or iTerm** (the *responsible process*).
   The first run shows the prompt "Terminal would like to use Bluetooth": click *Allow*. If it was denied: System
   Settings > Privacy & Security > Bluetooth, enable Terminal/iTerm, restart that app. To reset the decision and get the
   prompt again: `tccutil reset Bluetooth com.apple.Terminal` (iTerm: `com.googlecode.iterm2`), then restart the app.
3. Bluetooth must be on. CoreBluetooth reports its state asynchronously; the server waits up to 10 s for it. If it logs
   `Bluetooth never became ready` / `BluetoothDisabled (7)`, the state stayed *Unknown*: that normally means the
   permission was never granted (no prompt could be shown, e.g. started from ssh, a launchd job or another headless
   process) or Bluetooth is off. Start it from a Terminal window on the Mac's own desktop.
4. macOS reports a central only when it **subscribes** to a notifying characteristic (see the profile doc), so the
   `[connect]` line appears at step 7 of the checklist, not at step 3.
5. The test binary embeds an `Info.plist` with `NSBluetoothAlwaysUsageDescription`, which CoreBluetooth requires.

**Windows (WinRT)**
1. Visual Studio 2019+ with *Desktop development with C++* and a Windows 10/11 SDK, plus CMake. Windows 10 1703+.
2. Bluetooth on (Settings > Bluetooth & devices).
3. The adapter must support the **LE peripheral role** (many USB dongles and older drivers do not). Check:
   Device Manager > Bluetooth > your adapter > Properties > Details > property *Bluetooth radio supports Low Energy
   Peripheral Role*, value must be `True`. Without it `StartAdvertising` returns an error and the log shows it.
4. `BleServer.dll` is copied next to the executable by the build; run from the repo root through the script.

**Linux (BlueZ)**
1. Packages (see [LINUX_BACKEND.md](LINUX_BACKEND.md)): `sudo apt-get install cmake build-essential pkg-config libbluetooth-dev libdbus-1-dev libglib2.0-dev bluez`.
2. **BlueZ 5.50 or newer** (`bluetoothctl --version`; needs the GATT manager and LE advertising manager). Some builds only
   allow advertising with `bluetoothd -E` (experimental): add `-E` to `ExecStart` of the `bluetooth` service if
   registration is refused.
3. `bluetoothd` must run: `systemctl status bluetooth`; adapter powered: `bluetoothctl power on`, `rfkill unblock bluetooth`.
4. The backend registers objects on the **system D-Bus**. Membership of the `bluetooth` group usually suffices:
   `sudo usermod -aG bluetooth $USER`, log out and in. If registration is rejected, check
   `/etc/dbus-1/system.d/bluetooth.conf` and polkit rules, or run once with `sudo` to confirm it is a permission issue.
   Quick check: `busctl tree org.bluez` lists `/org/bluez/hci0`. Watch `journalctl -u bluetooth -f` during the test.

### nRF Connect checklist

Client: nRF Connect for Mobile (Android/iOS) or nRF Connect for Desktop > *Bluetooth Low Energy*, on a **different
device** than the one running the server. Start the server and wait for `Advertising as "BLE-TestServer"`. Fill each
column with PASS / FAIL / n/a and a note. UUIDs are shortened: `...0001` means `7b1e0001-5a6c-4f0e-9d3b-2c1a8e4f6b01`.

| # | Step | Expected | macOS | Windows | Linux |
|---|---|---|---|---|---|
| 1 | Start the server (`scripts/run_hw_test.*`) | Log: `Initialize: Success`, `Profile registered`, `Advertising as "BLE-TestServer"`, no `[error]` | | | |
| 2 | nRF Connect: scan | `BLE-TestServer` appears (if only an address shows, wait for the scan response or rescan) | | | |
| 3 | Connect | Service `7B1E0000-...` with 4 characteristics and the BleLink service `7B1E1000-...` with 3 (RX, TX, INFO) are listed. Log `[connect]` (macOS: only after step 7) | | | |
| 4 | Read `...0001` (single arrow) | Text `uptime=..s read#1`; read again: uptime and counter increase (`read#2`). Log `[read] served` | | | |
| 5 | Write text `Hello` to `...0002` (type UTF-8/Text) | Log: `write on write ... 5 B hex=48 65 6C 6C 6F text="Hello"` | | | |
| 6 | Write hex `DEADBEEF` to `...0002` (Byte array) | Log: `hex=DE AD BE EF`, text shown with replacement characters | | | |
| 7 | Enable notifications on `...0003` (triple arrow) | `heartbeat #N` every second, N increasing. Log `[connect]` (macOS) | | | |
| 8 | Type `n` + Enter in the server console | Extra notification `manual #1` arrives immediately (`#2` ...) and the log shows `[notify] sent "manual #1"` | | | |
| 9 | Enable notifications on `...0004`, write `ping` | Notification with value `ping`; log shows the write and `echo -> notify back`. Repeat with hex `010203` | | | |
| 10 | BleLink by hand, steps B1-B3 below (server started with `--echo-messages`) | HELLO answered, `hello` received with `CRC ok` and echoed, `delivered (ACK ok)` after your ACK | | | |
| 11 | Read INFO `...1003` | `01 00 02 00 00 00 01 04 01 00` (version 1, maxPacket 512, maxMessage 16 MiB, window 4, PROGRESS, timeout default) | | | |
| 12 | Disconnect in nRF Connect | Log `[disconnect]` (macOS: after unsubscribe); heartbeat stops | | | |
| 13 | Connect again, repeat steps 4, 7 and 9 | Everything works again without restarting the server | | | |
| 14 | Type `s` + Enter (while disconnected) | Log `Stop advertising: Success`; the device disappears from a fresh scan. `s` again: it comes back | | | |
| 15 | Type `q` (or Ctrl-C) | Log `Server shut down`, `Bye.`, exit code 0, no crash dialog | | | |

Notes:
- nRF Connect may cache the GATT table: use *Refresh services* or toggle Bluetooth after changing the server (the
  old Large characteristic `...0005` is gone).
- macOS shows a random per-session identifier for the central, not a MAC address.
- Record failures with the exact log line and attach the `logs/hw-test-*.log` file.

### BleLink by hand in nRF Connect (smoke test only)

nRF Connect can only do the **HELLO handshake and single-packet messages** by hand: every packet must be typed
as hex, with the right `seq` counter, and every message the server sends must be ACKed by hand within the
timeout. Multi-packet messages (anything above 8 bytes at the default MTU, above `packetSize - 12` in general),
throughput, flow control and loss repair **cannot** be tested this way: use the reference client (next section).

Start the server with `--echo-messages`. In nRF Connect, open service `7B1E1000-...`. Write everything below to
**RX `...1001`** as *Byte array*, with response (the normal *Write* in nRF Connect). Spaces are for reading only.
The test `scenario_gatt` replays exactly these bytes, so they are known to be correct.

| # | Do | Hex | Expected (TX notifications / log) |
|---|---|---|---|
| B0 | Enable notifications on TX `...1002` (triple arrow) | | Nothing yet; `p` lists the central as `handshaking (no HELLO yet)` |
| B1 | Write HELLO: version 1, maxPacket 244, maxMessage 1024, window 1, no PROGRESS, timeout 60 s | `01 00 01 F4 00 00 04 00 00 01 00 F0` | TX: server HELLO `01 00 01 00 02 00 00 00 01 04 01 00`. Log `[link] peer <id> READY: packet size 20 B` (or up to 244 after an MTU request), `T 60.00 s` |
| B2 | Write message "hello" (MSG_START seq 1, msgId 0, length 5, crc32 `3610A686`) | `02 01 00 00 05 00 00 00 86 A6 10 36 68 65 6C 6C 6F` | TX: ACK `04 01 00 00 00`, then the echo `02 02 00 00 05 00 00 00 86 A6 10 36 68 65 6C 6C 6F`. Log `[recv] message from <id> (wire id 0): 5 B, CRC ok (crc32=3610A686) ... text="hello"` |
| B3 | ACK the echo (ACK seq 2, msgId 0, status 0) | `04 02 00 00 00` | Log `[echo] message 1 to <id> delivered (ACK ok): 5 B` |
| B4 | Type `m hi` in the server console | | TX: `02 03 01 00 02 00 00 00 AC 2A 93 D8 68 69` (seq 3, msgId 1, "hi") |
| B5 | ACK it (ACK seq 3, msgId 1) | `04 03 01 00 00` | Log `[send] message 2 to <id> delivered (ACK ok): 2 B` |

Rules if you go further: your own packets count `seq` 0 (HELLO), 1, 2, ... (one per write, ACKs included); a
message must fit into one packet (`12 + n <= packetSize`); its CRC is CRC-32/IEEE of the payload, little-endian.
If you do not ACK a server message within T (60 s with the HELLO above), the server resends it up to 3 times and then
logs `[error] message ... failed: timeout`. Writing the HELLO again resets the session (seq starts over). A
wrong `seq` is not an error: the server just notes the gap.

### BleLink with the reference client (the real test)

The real BleLink test uses a protocol-correct client on a second machine: the Python reference client
`blelink-client` (`clients/python`, PR #12) or the platform samples (Android, iOS, MCU). Start the server with
`--echo-messages`, then on the other machine:

```bash
blelink-client --name BLE-TestServer echo-test 0 1 20 244 4k 64k 1M   # round trips, PASS/FAIL exit code
blelink-client --name BLE-TestServer send-blob 1M                      # client -> server throughput
blelink-client --name BLE-TestServer interleave 256k 16                # small message overtakes a large one
```

and in the server console `b 1M` (server -> client throughput) and `p` while a transfer runs. Record per client:
packet size and MTU from the `[link] ... READY` line, small-message round trip, 1 MB each direction with the
KB/s from the `delivered` / `[recv]` lines, interleaved small + large, and what happens on a disconnect in the
middle of a transfer (`[error] ... failed: disconnected` on the server, partial message discarded).

### Results

| Platform | Date | Commit | Client (nRF Connect version, device) | Result (PASS / FAIL, failing steps) | Notes |
|---|---|---|---|---|---|
| macOS | | | | | |
| Windows | | | | | |
| Linux | | | | | |

## (c) Unity sample

[samples/UnityBleSample](../samples/UnityBleSample/README.md) exposes the same GATT profile through the Unity
plugin (advertised name `BoostanBLE-Sample`) and has its own per-OS checklist. Run it after (b) passed on the
same machine: a failure in (c) that does not occur in (b) is in the C# wrapper, the plugin packaging or Unity.
