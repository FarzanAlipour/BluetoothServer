# UnityBleSample: manual BLE test project

A small Unity 2022.3 project that turns your computer into a BLE peripheral (GATT server) so you can test
the BluetoothServer plugin with **nRF Connect** on a phone or another PC.

**Supported targets: Linux, macOS and Windows.** The sample is the same on all three; only the plugin
binary and the Bluetooth setup differ (sections below).

- Unity **2022.3.62f3** (other 2022.3 LTS builds should work; `run_tests.*` default to this one).
- Advertised name: `BoostanBLE-Sample`.
- No C# is duplicated: `unity/` in this repo is a local UPM package (`unity/package.json`), referenced from
  `Packages/manifest.json` as `"com.boostan.bluetoothserver": "file:../../../unity"`.

```
Assets/Scripts/        BleSampleGatt.cs (UUIDs), BleSampleController.cs (server + on-screen UI)
Assets/Scenes/         BleSampleScene.unity (one GameObject with BleSampleController, IMGUI UI)
Assets/Editor/         BleSamplePostBuild.cs (macOS Info.plist key), BleSampleSceneBuilder.cs
Assets/Tests/EditMode/ automated tests
Assets/Plugins/<os>/   native binaries (built locally, not committed)
PluginMeta/            committed import settings for each plugin binary (see "Why PluginMeta/")
build_plugin.sh|ps1    build + install the native plugin
run_tests.sh|ps1       EditMode tests in batchmode
```

## GATT layout

Custom 128-bit UUIDs. Service `7b1e0000-5a6c-4f0e-9d3b-2c1a8e4f6b01`; each characteristic only changes the
`0000` part of the first group.

| Name | UUID | Properties | Behaviour |
|---|---|---|---|
| Service | `7b1e0000-...-6b01` | | |
| Read | `7b1e0001-...` | Read | Returns `uptime=12.3s read#5` (changes on every read). The provider runs on a native thread and uses no Unity API. |
| Write | `7b1e0002-...` | Write, Write w/o response | Bytes are logged as hex and as UTF-8. |
| Notify | `7b1e0003-...` | Read, Notify | `heartbeat #N` every 1 s while a client is connected/subscribed; `manual #N` from the **Send notify** button. |
| Echo | `7b1e0004-...` | Write, Write w/o response, Notify | Whatever you write comes back as a notification (round trip). |
| Large | `7b1e0005-...` | Write, Write w/o response, Notify | **Send large message** sends ~1000 bytes as 256-byte `BleMessageChunker` chunks (13-byte header + payload) as notifications. A written packet is fed to the chunker (an ACK `02 ...` marks a chunk as acknowledged). |

(Full UUIDs: replace `...` with `-5a6c-4f0e-9d3b-2c1a8e4f6b01`.)

### Large message and nRF Connect

A chunk is 256 bytes, so the notification only arrives intact when the **ATT MTU is at least 259**.
In nRF Connect (Android) request MTU 517 after connecting (menu next to the device name > *Request MTU*);
iOS negotiates it automatically. With the default MTU 23 chunks are truncated by the link, which is a
limitation of the client setup, not a bug. nRF Connect shows each chunk as a separate notification; it
does not reassemble them, and typing a valid ACK by hand is optional. Header (little-endian):
`01` data | message id (4 B) | chunk index (4 B) | total chunks (4 B) | payload. Reassembly and retransmission
are covered by the native `ctest` suite, not by this manual test.

### "Connected" on each OS

The native callbacks report a central as *connected* differently per backend. On **macOS** CoreBluetooth
does not tell a peripheral about a plain connection, so the status line shows a device only after the client
**enables notifications** (and shows it gone when it unsubscribes). On Linux and Windows the connection
itself is reported. The heartbeat starts when a device is listed.

## Setup

Common: [CMake](https://cmake.org) >= 3.15 and a C++17 compiler, Unity 2022.3.62f3.
**Close the Unity Editor before rebuilding the plugin**: an Editor that has loaded the native library keeps
it locked/loaded until it quits.

### macOS (CoreBluetooth)

1. `xcode-select --install`, then `brew install cmake` (or the cmake.org installer).
2. Build: `./build_plugin.sh` (creates the **universal arm64 + x86_64** `Assets/Plugins/macOS/libBleServer.dylib`
   and installs its import settings).
3. Open this folder in Unity Hub (version 2022.3.62f3), open `Assets/Scenes/BleSampleScene.unity`, press Play.
4. **Bluetooth permission.** In the Editor the permission belongs to the app that hosts the code, i.e.
   **Unity** (or Unity Hub if you start it from there). The first call to CoreBluetooth shows the macOS
   prompt; click *Allow*. If you denied it: *System Settings > Privacy & Security > Bluetooth*, enable Unity
   (and Unity Hub) and restart the Editor, or reset the decision with
   `tccutil reset Bluetooth com.unity3d.UnityEditor5.x` (and `com.unity3d.unityhub` for the Hub).
5. Standalone build (*File > Build Settings > macOS*): the Editor post-build step
   (`Assets/Editor/BleSamplePostBuild.cs`) adds `NSBluetoothAlwaysUsageDescription` to the app's
   `Contents/Info.plist`; without that key macOS terminates the app at the first Bluetooth call. The built
   app then asks for permission itself (*Privacy & Security > Bluetooth*).

### Windows (WinRT)

1. Install Visual Studio 2022 (workload *Desktop development with C++*, Windows 10/11 SDK) and CMake.
2. In a PowerShell: `.\build_plugin.ps1` (creates `Assets\Plugins\Windows\x86_64\BleServer.dll`).
   If scripts are blocked: `powershell -ExecutionPolicy Bypass -File .\build_plugin.ps1`.
3. Open the folder in Unity Hub, open `BleSampleScene`, press Play.
4. **Bluetooth requirements.** Bluetooth must be **On** (Settings > Bluetooth & devices). The adapter must
   support the **LE peripheral role** (Bluetooth 4.0+ with a driver that implements it; many USB dongles do
   not). Check in Device Manager > Bluetooth > your adapter > Properties > Details > property
   *Bluetooth radio supports Low Energy Peripheral Role*; it must be `True`. If the role is unsupported
   (or Bluetooth is off), `StartAdvertising` or `Initialize` returns an error that the log shows.
5. Standalone build: the plugin is a normal (unpackaged) Win32 DLL calling WinRT APIs, which needs **no
   app-manifest capability**. A packaged (MSIX/UWP) build would need the `bluetooth` device capability in
   its `Package.appxmanifest`; that route is not used by this sample.

### Linux (BlueZ)

1. Packages (Debian/Ubuntu; see `docs/LINUX_BACKEND.md`):
   `sudo apt-get install cmake build-essential pkg-config libbluetooth-dev libdbus-1-dev libglib2.0-dev bluez`.
2. `./build_plugin.sh` creates `Assets/Plugins/Linux/x86_64/libBleServer.so` (x86_64 only).
3. Unity Hub for Linux, open the folder, open `BleSampleScene`, press Play.
4. **Requirements.** `bluetoothd` running (`systemctl status bluetooth`), BlueZ **5.50 or newer**
   (`bluetoothctl --version`; needs the GATT manager and LE advertising manager), adapter powered
   (`bluetoothctl power on`, `rfkill unblock bluetooth`). The plugin talks to BlueZ over the **system D-Bus**:
   your user must be allowed to own/register objects there. On most distributions membership in the
   `bluetooth` group is enough (`sudo usermod -aG bluetooth $USER`, then log out and in); if registration is
   rejected, check `/etc/dbus-1/system.d/bluetooth.conf` and your distribution's polkit rules, or run the
   Editor as a user covered by them. Quick check: `busctl tree org.bluez` must list `/org/bluez/hci0`.
   Some BlueZ builds only allow advertising with `bluetoothd -E` (experimental); the plugin reports a native
   error in the on-screen log when BlueZ refuses a call.

### Why PluginMeta/

Unity **deletes** a `.meta` whose binary is missing, and the binaries are not committed. So a committed
`Assets/Plugins/<os>/<binary>.meta` would vanish the first time you open the project on another OS. The
canonical import settings therefore live in `PluginMeta/` (committed), restricted to the plugin's own OS
(Editor + Standalone only; `Any` and the other OSes disabled), and `build_plugin.*` / `run_tests.*` copy the
host OS's file next to the binary. The copies are git-ignored. The EditMode tests verify the restriction.

## Automated tests (EditMode, batchmode)

`./run_tests.sh` (macOS/Linux) or `.\run_tests.ps1` (Windows) builds the plugin if missing, runs Unity with
`-batchmode -nographics -runTests -testPlatform EditMode` and prints the result per test. Override the Editor
path with `UNITY=/path/to/Unity` (`$env:UNITY` on Windows). They check: the project compiles, the host OS's
native plugin loads (API version + build info via P/Invoke, create/destroy; no Bluetooth is touched), the
GATT UUIDs are valid and distinct, the advertised name fits, every plugin's import settings are restricted to
its own OS (all three, from `PluginMeta/`), and the macOS Info.plist post-build step. Real BLE cannot be
automated; use the checklist below.

## Manual test checklist (nRF Connect)

Client: nRF Connect for Mobile (Android/iOS) or nRF Connect for Desktop > *Bluetooth Low Energy*, on a
**different device** than the one running Unity. Start the sample (Play), wait until the log shows
`Initialize: Success`, then work through the table. Fill the result columns with PASS / FAIL / n/a and a note.

| # | Step | Expected | macOS | Windows | Linux |
|---|---|---|---|---|---|
| 1 | Press **Start advertising** | Log: `Start advertising as "BoostanBLE-Sample": Success`; status `advertising=True` | | | |
| 2 | nRF Connect: scan | `BoostanBLE-Sample` appears (if only the address shows, wait for the scan response or refresh) | | | |
| 3 | Tap **Connect** | Connected; the service `7B1E0000-...` and 5 characteristics are listed. Status line lists a device (macOS: only after step 7) | | | |
| 4 | Read characteristic `...0001` (single-arrow button) | Value `uptime=..s read#1`, shown as text; read again: uptime and counter increase. Unity log: `Read served` | | | |
| 5 | Write text to `...0002` (type `Hello`, format UTF-8/Text) | Unity log: `Write 5 B hex=48 65 6C 6C 6F text="Hello"` | | | |
| 6 | Write hex to `...0002` (`DEADBEEF`, format Byte array) | Log: `hex=DE AD BE EF`, text shows replacement characters | | | |
| 7 | Enable notifications on `...0003` (triple-arrow button) | nRF shows `heartbeat #N` every second, N increasing; status shows the device | | | |
| 8 | Press **Send notify** in Unity | An extra `manual #1` notification arrives immediately (again: `#2`, ...) | | | |
| 9 | Enable notifications on `...0004`, write `ping` to it | Notification with value `ping` (and the Unity log shows the write and `-> notify back`); repeat with hex `01 02 03` | | | |
| 10 | Request MTU 517, enable notifications on `...0005`, press **Send large message** | 5 notifications of 256 bytes (the last is shorter): first byte `01` (data chunk), the same 4-byte message id in each, chunk index `00 00 00 00`, `01 00 00 00`, ... and total chunks `05 00 00 00`. Log: `Send large message (...): Success` | | | |
| 11 | Disconnect in nRF Connect | Log: `Central disconnected`; status `connected=none`; heartbeat stops | | | |
| 12 | Connect again, repeat steps 4, 7 and 9 | Everything works again with no restart | | | |
| 13 | Press **Stop advertising** (while disconnected) | Log: `Stop advertising: Success`; the device disappears from a fresh scan | | | |
| 14 | Stop Play mode | No errors in the Console; Unity does not hang | | | |

Notes for the tester:
- nRF Connect on iOS/Android may cache the GATT table; after changing the sample use *Refresh services* or toggle Bluetooth.
- On macOS the client's address is a random per-session identifier, not a MAC.
- Record failures with the exact on-screen log line. On Linux also watch `journalctl -u bluetooth -f`.

### Results

| Platform | Date | Unity | Client | Result | Notes |
|---|---|---|---|---|---|
| macOS | | 2022.3.62f3 | | | |
| Windows | | 2022.3.62f3 | | | |
| Linux | | 2022.3.62f3 | | | |
