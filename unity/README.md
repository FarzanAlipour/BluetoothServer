# BluetoothServer Unity plugin

Native BLE GATT peripheral for Unity on Windows, Linux and macOS. `BleServer.cs` is the C# wrapper
over the native library built from this repository.

## Install

1. Build the native library with CMake and let it copy the binary into your project:

   ```bash
   cmake -S . -B build -DUNITY_PROJECT_PATH=/path/to/YourUnityProject
   cmake --build build --config Release
   ```

   Or copy the binary by hand:

   | OS | Binary | Destination in your project |
   |---|---|---|
   | Windows (x86_64) | `BleServer.dll` | `Assets/Plugins/Windows/x86_64/` |
   | Linux (x86_64) | `libBleServer.so` | `Assets/Plugins/Linux/x86_64/` |
   | macOS (arm64 + x86_64, universal) | `libBleServer.dylib` | `Assets/Plugins/macOS/` |

   Linux build prerequisites: `libbluetooth-dev libdbus-1-dev libglib2.0-dev pkg-config`.
2. Copy `unity/BleServer.cs` into `Assets/`. In the Unity inspector select the plugin and enable only the
   platform it belongs to. Copy the real file, not a symlink (macOS).
3. Optional: `BleServerTestGui.cs` is a sample scene script.

## Use

```csharp
var server = new BluetoothServer.Unity.BleServer();
server.CharacteristicWritten += (device, svc, chr, data) => { /* main thread */ };
server.Initialize(BleServerConfig.Create("My Device"));
server.AddService("180F");
server.AddCharacteristic("180F", "2A19", CharacteristicProperty.Read | CharacteristicProperty.Write,
                         CharacteristicPermission.Read | CharacteristicPermission.Write);
server.SetWriteCallback("180F", "2A19");
server.StartAdvertising();
// MonoBehaviour.Update():  server.DispatchEvents();
// OnDestroy():             server.Dispose();
```

## Threading contract (important)

Native callbacks come from native threads. The wrapper queues them; **you must call
`server.DispatchEvents()` once per frame from the Unity main thread**. `ConnectionChanged`, `Error` and
`CharacteristicWritten` run on that thread, so they may use Unity APIs. The read provider passed to
`SetReadCallback` is the exception: it runs on a native thread while the central waits, so keep it fast
and thread-safe and do not touch Unity objects in it. After `Dispose()` no further events are raised.

## Messages of any size: BleLink (native API v3)

`BleLink` sends and receives messages of any size (0 bytes up to 16 MiB by default) with each connected
central, using the BleLink v1 protocol (`docs/PROTOCOL.md`, ABI contract in `docs/BLELINK_ABI.md`). It handles
flow control, MTU changes, CRC and loss repair.

```csharp
server.Initialize(BleServerConfig.Create("My Device"));
var link = new BluetoothServer.Unity.BleLink(server);   // before StartAdvertising; one per server
link.PeerReady += (device, info) => link.Send(device, Encoding.UTF8.GetBytes("hi"));
link.MessageReceived += (device, data) => { /* main thread */ };
link.MessageFailed += (device, id, reason, ackStatus) => { };
server.StartAdvertising();
// MonoBehaviour.Update():  server.DispatchEvents(); link.Update();
// OnDestroy():             link.Dispose(); server.Dispose();
```

`link.Update()` raises `PeerReady`, `PeerGone`, `MessageReceived`, `MessageSent`, `MessageFailed` and `Progress`
on the calling thread. The link polls native events, so it involves no native->managed callbacks.
`Send`/`TrySend` take one central (`ReadyPeers` lists them), `SendToAll` sends to every READY central. Errors are
`PeerNotReady`, `MessageTooLarge` and `NotInitialized` (the server was shut down; create a new link after
re-initializing).

Deprecated: `SendLargeMessage`, `Chunker` and `RetransmitTimedOut` (the fixed 256-byte chunker: needs
ATT_MTU >= 259, no flow control) still work but are `[Obsolete]`.

## Expected targets (assumed, needs owner confirmation)

Unity 2021.3 LTS and 2022.3 LTS, Editor and standalone players; macOS both architectures, Windows and
Linux x86_64. IL2CPP: callbacks are static `[MonoPInvokeCallback]` methods held in static fields, which is
the pattern IL2CPP requires; this is not verified by CI. Which Unity versions/targets must be supported
is an open question.

## What is and is not tested

CI proves that the native library compiles on all three OSes, that the C ABI behaves correctly against a
fake backend (including `BleLink_*`, `tests/test_blelink_abi.cpp`), and (`scripts/check_abi_parity.py`) that
every native function has a matching `[DllImport]`. The C# struct layouts (`BleLinkConfig` 24 bytes,
`BleLinkPeerInfo` 56, the event 368) must match `static_assert`s in `src/common/BleLinkAbi.cpp`.
It does not run Unity or real Bluetooth. Test in the Editor and in a built player on each OS by hand.
