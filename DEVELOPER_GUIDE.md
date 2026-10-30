# Developer Guide

For people changing BluetoothServer. User-facing docs: `README.md`, `unity/README.md`.
The BlueZ backend has its own reference: `docs/LINUX_BACKEND.md`.

## 1. Layers

```
 Unity C# (unity/BleServer.cs)  ──  [DllImport] one-to-one with the C ABI
              │
 C ABI  (include/IBleServer.h, implemented in src/common/BleServer.cpp)
   - validates every argument, maps results to BleError ints, catches every exception
   - handle registry: null / destroyed handles are rejected, never dereferenced
   - callback gating + "no callback under a lock" dispatch
   - chunker wrapper (BleChunker_*): reassembly, ACK handling, retransmission
              │
 Common logic (src/common: BleUtilities.h validators, include/BleMessageChunker.h)
   - no OS includes, no OS #ifdefs  (enforced by scripts/check_layers.sh)
              │
 IBleServer  (C++ interface, include/IBleServer.h)
     ┌────────────────┬──────────────────┬───────────────────┬──────────────────────┐
 Windows (WinRT)   Linux (BlueZ/D-Bus)   macOS (CoreBluetooth)   FakeBleServer (tests/fake)
 src/platforms/    src/platforms/        src/platforms/          in-memory, used by ctest
 windows           linux                 macos
```

Rule of thumb: logic that does not need the OS belongs in `src/common` and is tested against the
fake backend. A platform backend only translates `IBleServer` calls to the native stack.
The platform string reported by `BleServer_GetBuildInfo()` is the compile definition
`BLE_BUILD_PLATFORM`, set per OS in `CMakeLists.txt`.

## 2. Build, test, verify

```bash
# all platforms (Windows: add -C Release to ctest)
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

# sanitizers (macOS / Linux)
cmake -S . -B build-san -DENABLE_SANITIZERS=ON && cmake --build build-san
ctest --test-dir build-san --output-on-failure

# repo checks (also run in CI)
sh scripts/check_hygiene.sh        # no build output / .DS_Store tracked
sh scripts/check_layers.sh         # no OS code in src/common
python3 scripts/check_abi_parity.py  # every BLE_API function has a matching [DllImport]
```

Platform prerequisites: Windows needs Visual Studio with the C++ workload and a Windows 10 SDK;
Linux needs `libbluetooth-dev libdbus-1-dev libglib2.0-dev pkg-config`; macOS needs the Xcode tools.

Tests (`tests/`, all run by `ctest`, none need Bluetooth hardware):

| ctest name | file | covers |
|---|---|---|
| `chunker` | `test_chunker.cpp` | `BleMessageChunker` only (no server, no backend) |
| `abi` | `test_abi.cpp` | null/invalid handles on every entry point, UUID/config validation, lifecycle (double shutdown, destroy while initialized), no callbacks after shutdown/destroy, concurrency smoke, destroy during a running callback |
| `chunker_abi` | `test_chunker_abi.cpp` | empty / 1 chunk / +1 byte / 1000 B / max size, lost ACK and lost chunk retransmission, duplicate and out-of-order ACKs and chunks, retry bound and failure, bounded memory, stale handles, callbacks that re-enter the API |
| `notify_queue` | `test_notify_queue.cpp` | `NotificationQueue` (FIFO, busy/ready, bound drops oldest, discard), and through the C ABI with the fake's flow-control model: 5-chunk burst into a full OS queue is delivered in order, value captured at notify time, overflow reported, cleared on last unsubscribe / Shutdown / remove, error callback runs unlocked |

Sanitizer note: on macOS the sanitizer build is single-architecture on purpose (the x86_64 slice of an
ASan binary hangs at startup under Rosetta). `-DBLE_SANITIZER_KINDS=thread` selects ThreadSanitizer
where the toolchain supports it.

## 3. Lifecycle and locking rules

These are the invariants `BleServer.cpp` guarantees; keep them when you edit it.

1. **Handles.** `BleServer_Create` / `BleChunker_Create` register the object; every entry point looks the
   handle up and holds a `shared_ptr` for the duration of the call. Unknown, null or destroyed handles
   return `InvalidParameter` (or false / 0 / -1 for non-int functions). A handle is valid from `Create`
   to `Destroy`; using a stale pointer after the address was reused by a new `Create` is undefined
   behaviour (the C ABI cannot detect that).
2. **No callback under a lock.** Callbacks are copied under `CallbackState::mutex`, the lock is released,
   then the user function runs (`DeliverConnection`, `DeliverError`). Never call user code while holding
   a mutex, in any layer, including backends.
3. **No callbacks after shutdown, and none still running.** Every user callback runs inside a
   `CallbackScope`, which counts it in `CallbackState::inFlight` only while `active` is true.
   `Shutdown`/`Destroy` take the lifecycle lock, clear `active`, then **wait until `inFlight` is 0**
   (`DeactivateAndDrain`) before stopping the backend. When they return, no user callback is running or
   will start, so the caller (e.g. Unity `Dispose`) may free `GCHandle`s/userData. Exception: a callback
   that itself calls `Shutdown`/`Destroy` cannot wait for itself (thread-local depth check); it only clears
   `active`. The lambdas own a `shared_ptr` to the state, never a pointer to the wrapper.
4. **Shutdown is idempotent and joins its threads** (backend responsibility; `IBleServer::Shutdown`
   must be safe to call repeatedly and from the destructor).
5. **No exception crosses the C boundary.** Every entry point is wrapped in `GuardInt/GuardVoid/GuardRet`.
6. **Bounded memory.** Chunker: at most 8 partially received, 8 un-ACKed sent and 16 completed-but-unread
   messages are kept (oldest dropped); a chunk header claiming more than 1000 chunks is rejected.

### Chunker retransmission contract

`BleChunker_SendMessage` sends all chunks and remembers the message until every chunk is ACKed
(ACKs are fed to `BleChunker_ProcessIncoming`; duplicate, out-of-order and unknown ACKs are harmless).
`BleChunker_CleanupTimedOut(chunker, timeoutMs)` resends the oldest unacknowledged chunk of every
message whose last transmission is at least `timeoutMs` old and returns the number of chunks resent.
After 5 resends of the same chunk the message is failed: it is released and reported through the
server's error callback (`BleError::Unknown`, `"chunk N not acknowledged"`). The receiver ACKs every
data chunk (also duplicates) and delivers a message exactly once. The host application calls
`CleanupTimedOut` periodically (the Unity wrapper exposes `RetransmitTimedOut`). Limits:
243 payload bytes per chunk, 1000 chunks, 243 000 bytes per message.

### Notification flow control

`NotifyCharacteristicChanged` captures the characteristic's current value. When the OS transmit
queue is full (macOS: `updateValue:forCharacteristic:onSubscribedCentrals:` returns NO) the backend
keeps it in a `NotificationQueue` (`src/common/NotificationQueue.h`, max 256 values) and returns
Success ("sent or queued"). New values never overtake queued ones. On
`peripheralManagerIsReadyToUpdateSubscribers:` the queue is sent in order until the OS is full again.
Overflow drops the oldest value and reports `BleError::Unknown` through the error callback; backlog
start and drain are reported with `BleError::Success` (they show up in hw-test logs, plus `NSLog`).
The queue is cleared on Shutdown, on removing the characteristic/service, and when the last central
unsubscribes from the characteristic. Reports are delivered after the backend lock is released.
`FakeBleServer::SetTransmitCapacity` / `SimulateReadyToUpdate` / `SimulateSubscription` model this
for tests. Only the macOS backend uses it today; Windows and Linux send immediately.

### Per-central transport (`NotifyPeer`, capability `PeerNotify`)

What a message engine (BleLink, `docs/PROTOCOL.md`) needs; contract in `include/IBleServer.h`.
`NotifyPeer` sends one notification to one subscribed central and never queues: `Busy` means nothing
was sent and a `ReadyToSend` callback follows. Subscribe/unsubscribe and the max notification payload
(ATT_MTU - 3) are reported per central. Writes (with and without response) reach the write callback
one at a time, in OS order, with the writing central's id, and without a backend lock held.

| | Per-central send | Busy / ready | Max payload | Subscription events |
|---|---|---|---|---|
| macOS | `updateValue:...onSubscribedCentrals:@[central]` | NO -> Busy; `peripheralManagerIsReadyToUpdateSubscribers:` -> `ReadyToSend("")` (one OS queue for all centrals; legacy queued values go first) | `CBCentral.maximumUpdateValueLength` (refreshed on subscribe/write) | `didSubscribe/didUnsubscribe` |
| Windows | `NotifyValueAsync(value, GattSubscribedClient)` | one call in flight per central (spec 6.2), a second -> Busy; completion -> `ReadyToSend(id)` | `GattSession.MaxPduSize - 3`, `MaxPduSizeChanged` | diff of `SubscribedClients` per characteristic |
| Linux | one message on the central's `AcquireNotify` socket (`SOCK_SEQPACKET`) | `EAGAIN` -> Busy; `POLLOUT` -> `ReadyToSend(id)` | `"mtu"` option of `AcquireNotify` - 3 | `AcquireNotify` = subscribe, socket hang-up = unsubscribe |

Device ids: macOS = `CBCentral.identifier`, Windows = hash of the WinRT device id (`DEV-...`), Linux =
BlueZ device object path (same id as the connection callback). Linux refuses empty notifications (an
empty message reads as end-of-stream to BlueZ). `FakeBleServer` models all of it
(`SetPeerTransmitCapacity`, `SimulatePeerReady`, `SimulateReadyToUpdate`, `SimulateMtu`,
`SimulateSubscription`, `SimulateWriteEx`); see `tests/test_peer_notify.cpp`.

**BlueZ version caveat.** Per-device `AcquireNotify` sockets exist since BlueZ 5.69, and data written
to one socket goes to that device only since BlueZ 5.76 (commit a08ec1a "Fix sending notification to
all devices"). On 5.69-5.75 a `NotifyPeer` to one central also reaches the other subscribers of that
characteristic; before 5.69 only the first subscriber gets a socket (others: `NotSubscribed`). With a
single connected central (the common game setup) every version behaves correctly. Also, the socket
only back-pressures when bluetoothd stops reading it; bluetoothd's own ATT queue is unbounded, so
Busy on Linux is rarer than on macOS/Windows.

**Not verified on hardware yet** (CI only compiles Windows/Linux and runs the fake-backend tests):
- macOS: a burst of `NotifyPeer` gets Busy and resumes after `peripheralManagerIsReadyToUpdateSubscribers:`
  without loss or reordering; `maximumUpdateValueLength` matches the negotiated MTU (e.g. 182/244/509).
- Windows: `MaxPduSize` is the ATT MTU and `MaxPduSizeChanged` fires after the exchange; the
  `NotifyValueAsync` completion arrives per notification; write ids are stable across read/write/subscribe;
  writes without response arrive in order under load (`OrderedDelivery`).
- Linux: BlueZ calls `AcquireNotify` (not `StartNotify`) once `NotifyAcquired` is exported, with
  `device` and `mtu`; hang-up arrives on CCC disable and on disconnect; `EAGAIN`/`POLLOUT` really
  happens under a burst; behaviour with two centrals on BlueZ < 5.76 (see caveat).
- All: subscribe/unsubscribe/MTU callbacks fire once per change and in the order
  connection -> max payload -> subscription.

## 4. How to add things

### A characteristic (or descriptor) for your game
No native change needed: call `AddService` / `AddCharacteristic(Ex)` / `AddDescriptor` (C#:
`BleServer.AddCharacteristic`). To add a *built-in* characteristic in native code, add it where the
server is configured, then cover it in `tests/test_abi.cpp` against the fake backend
(`FakeBleServer::SimulateWrite` plays the central).

### A feature / ABI function
Touch exactly these places, in this order:
1. `include/IBleServer.h`: add the `BLE_API` function (additions only) and, if it needs the backend, a
   virtual on `IBleServer`.
2. `src/common/BleServer.cpp`: implement it with the standard shape (registry lookup, argument checks,
   guard) and add the backend call.
3. Every backend in `src/platforms/*` and `tests/fake/FakeBleServer.*` implements the new virtual.
4. `unity/BleServer.cs`: add the matching `[DllImport]` (and a wrapper method).
5. Tests: a null-handle row in `TestNullHandles` (`tests/test_abi.cpp`) plus a behaviour test.
6. Bump the API version (section 5) and update `README.md`.
`scripts/check_abi_parity.py` fails CI if step 4 is forgotten.

### A platform backend
1. Create `src/platforms/<os>/BleServer<Os>.cpp` implementing `IBleServer` and
   `std::unique_ptr<IBleServer> BleServer::CreatePlatformServer()`. Keep OS includes inside it.
2. Add a CMake branch in `CMakeLists.txt` (sources, link libraries, `BLE_BUILD_PLATFORM`).
3. Add a CI job in `.github/workflows/ci.yml` that builds and runs `ctest` on that OS.
4. Add install notes to `README.md` and `unity/README.md`.
5. Run `scripts/check_layers.sh`: nothing OS-specific may leak into `src/common`.

## 5. ABI versioning and deprecation policy

- `BleServer_GetApiVersion()` returns the integer API version (currently **3**: v3 added `BleLink_*`, see
  `docs/BLELINK_ABI.md`, and deprecated `BleChunker_*`; `GetBuildInfo` prints it).
  The constant is `kApiVersion` in `src/common/BleServer.cpp` and must match the `api=` text in
  `BleServer_GetBuildInfo`.
- **Any change to `include/IBleServer.h`'s C ABI bumps the version by one.** Only additions are allowed:
  new functions, new enum values at the end, new trailing struct fields guarded by the `size` member.
  Existing signatures, struct layouts and `BleError` values never change within a major line.
- Unity code can gate on `BleServer.NativeApiVersion` to refuse an older native library.
- **Deprecation:** mark a function `// DEPRECATED since api N: use X` in the header and in
  `unity/BleServer.cs` (`[Obsolete]`), keep it working for at least one further version, and remove it only
  together with a major release that is announced in the release notes. Behavioural tightening of
  argument validation (rejecting values that were never valid) is not a break; document it.
- Tests assert the current version (`test_abi.cpp`); update them with the bump.

## 6. Release checklist

- [ ] `sh scripts/check_hygiene.sh && sh scripts/check_layers.sh && python3 scripts/check_abi_parity.py`
- [ ] Local macOS: `ctest` green, and the `-DENABLE_SANITIZERS=ON` build green
- [ ] GitHub Actions green on Windows, Linux, macOS and both sanitizer jobs for the release commit
- [ ] API version bumped if the header changed; `README.md`, `unity/README.md` and this guide are current
- [ ] Manual hardware smoke test per platform (advertise, connect with nRF Connect, read/write/notify,
      a chunked transfer with a dropped ACK) — CI cannot do this
- [ ] Unity: build the plugin into a test project (Editor + a standalone/IL2CPP player) and run the sample
- [ ] Tag the commit and attach the three platform binaries

## 7. Backend notes (from the former status report)

**Linux (BlueZ via D-Bus/GIO).** The backend exports a GATT application under `/com/example/ble`
(ObjectManager) with one `GattService1` per service, one `GattCharacteristic1` per characteristic and a
`LEAdvertisement1` at `/advertisement`. Startup: create the objects, register the ObjectManager, call
`GattManager1.RegisterApplication()` on the adapter (`/org/bluez/hci0`); BlueZ then calls
`GetManagedObjects()`. Advertising: create `LEAdvertisement1` (Type, ServiceUUIDs, LocalName) and call
`LEAdvertisingManager1.RegisterAdvertisement()`. `NotifyCharacteristicChanged` notifications are
`PropertiesChanged` signals (broadcast); `NotifyPeer` uses the per-central `AcquireNotify` socket
(characteristics with notify/indicate export `NotifyAcquired`, see section 3). Needs BlueZ 5.x. See `docs/LINUX_BACKEND.md` for the code walk-through.

**Windows (WinRT)** uses `GattServiceProvider`; `RemoveCharacteristic` unregisters handlers but the WinRT
database only drops it when the service is removed. **macOS** uses `CBPeripheralManager`; it needs the
peripheral role and, when packaged, the Bluetooth usage descriptions/entitlements.

**Known limits.** Forced central disconnect is not generally possible in peripheral mode; descriptor
support varies by platform; hardware behaviour is manual-test only.
