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
| `scenario_gatt` | `tests/test_scenario_gatt.cpp` | the GATT test profile, end to end against the fake backend |

`scenario_gatt` compiles the **same server code as the CLI** (`examples/ble_test_server/TestServerApp.cpp`) and plays the
central through the fake backend's `Simulate*` hooks: init, add profile, advertise, connect, read (value format),
write (hex + UTF-8 logged), subscribe/heartbeat notifies, echo round trip, a chunked large message reassembled in both
directions, disconnect, reconnect, stop advertising, shutdown. Sanitizer run (macOS/Linux):

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
`--name NAME` (default `BLE-TestServer`), `--heartbeat-ms N` (default 1000), `--run-seconds N`.
Manual build: `cmake -S . -B build && cmake --build build --target ble_test_server`, binary in `build/examples/`.

Commands on stdin: `n` send a notify now, `l` send the large message, `s` stop/start advertising, `q` quit.
Ctrl-C also shuts down cleanly. Wait for `Advertising as "BLE-TestServer"`; every event is logged with a timestamp.
`[error]` lines carry the `BleError` code and name, e.g. `BluetoothDisabled (7)`.

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
| 3 | Connect | Service `7B1E0000-...` and 5 characteristics are listed. Log `[connect]` (macOS: only after step 7) | | | |
| 4 | Read `...0001` (single arrow) | Text `uptime=..s read#1`; read again: uptime and counter increase (`read#2`). Log `[read] served` | | | |
| 5 | Write text `Hello` to `...0002` (type UTF-8/Text) | Log: `write on write ... 5 B hex=48 65 6C 6C 6F text="Hello"` | | | |
| 6 | Write hex `DEADBEEF` to `...0002` (Byte array) | Log: `hex=DE AD BE EF`, text shown with replacement characters | | | |
| 7 | Enable notifications on `...0003` (triple arrow) | `heartbeat #N` every second, N increasing. Log `[connect]` (macOS) | | | |
| 8 | Type `n` + Enter in the server console | Extra notification `manual #1` arrives immediately (`#2` ...) and the log shows `[notify] sent "manual #1"` | | | |
| 9 | Enable notifications on `...0004`, write `ping` | Notification with value `ping`; log shows the write and `echo -> notify back`. Repeat with hex `010203` | | | |
| 10 | Request MTU 517, enable notifications on `...0005`, type `l` | 5 notifications (4 x 256 B, the last 41 B), first byte `01`, same 4-byte message id, chunk index `00000000`, `01000000` ..., total `05000000`. Log `[large] sending 1000 bytes as 5 chunks` | | | |
| 11 | Write an ACK to `...0005` (hex `02` + message id from step 10 + `00000000` + `00000000`) | Log `[large] ACK for message <id> chunk 0 (1/5 chunks acknowledged)`. Optional | | | |
| 12 | Disconnect in nRF Connect | Log `[disconnect]` (macOS: after unsubscribe); heartbeat stops | | | |
| 13 | Connect again, repeat steps 4, 7 and 9 | Everything works again without restarting the server | | | |
| 14 | Type `s` + Enter (while disconnected) | Log `Stop advertising: Success`; the device disappears from a fresh scan. `s` again: it comes back | | | |
| 15 | Type `q` (or Ctrl-C) | Log `Server shut down`, `Bye.`, exit code 0, no crash dialog | | | |

Notes:
- Step 10: chunks 1-4 are 256 B (13 B header + 243 B payload); the last is 13 + 28 = 41 B. With the default MTU 23
  chunks are truncated by the link: request MTU first (iOS negotiates automatically).
- nRF Connect may cache the GATT table: use *Refresh services* or toggle Bluetooth after changing the server.
- macOS shows a random per-session identifier for the central, not a MAC address.
- Record failures with the exact log line and attach the `logs/hw-test-*.log` file.

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
