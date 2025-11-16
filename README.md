# BluetoothServer

A cross-platform C++ library for creating Bluetooth Low Energy (BLE) GATT servers on Windows and Linux. Designed for Unity integration but can be used in any C/C++ application.

## Features

✅ **Cross-Platform**: Windows (WinRT) and Linux (BlueZ/D-Bus)
✅ **GATT Server**: Full support for services and characteristics
✅ **Advertising**: LE Advertisement with service UUIDs and local name
✅ **Callbacks**: Read, write, and connection event callbacks
✅ **Message Chunking**: Automatic fragmentation for large messages (>243 bytes)
✅ **Thread-Safe**: All operations properly synchronized
✅ **Unity Ready**: C API for easy integration with Unity and C#
✅ **Input Validation**: Comprehensive UUID, value, and parameter validation
✅ **Connection Tracking**: Monitor device connections and disconnections

## Platform Support

| Platform | Status | Implementation | Requirements |
|----------|--------|----------------|--------------|
| **Windows** | ✅ Complete | WinRT C++/17 | Windows 10+ with Bluetooth LE support |
| **Linux** | ✅ Complete | BlueZ 5.x via D-Bus/GIO | BlueZ 5.x, GLib 2.x, D-Bus |
| **macOS** | ❌ Not yet | N/A | Planned for future release |

## Quick Start

### Building on Windows

```bash
# Using Visual Studio
build-windows.bat

# Or using CMake
mkdir build && cd build
cmake ..
cmake --build . --config Release
```

### Building on Linux

```bash
# Install dependencies
sudo apt-get install libbluetooth-dev libdbus-1-dev libglib2.0-dev libgio2.0-cil-dev

# Build
mkdir build && cd build
cmake ..
make
```

### Basic Usage (C++ API)

```cpp
#include "IBleServer.h"

using namespace BleServer;

// Create server
auto server = IBleServer::Create();

// Configure and initialize
ServerConfig config;
config.deviceName = "My BLE Device";
config.maxConnections = 5;
server->Initialize(config);

// Add a service
ServiceConfig serviceConfig;
serviceConfig.uuid = "0000180a-0000-1000-8000-00805f9b34fb"; // Device Information Service
serviceConfig.isPrimary = true;
server->AddService(serviceConfig);

// Add a characteristic
CharacteristicConfig charConfig;
charConfig.uuid = "00002a29-0000-1000-8000-00805f9b34fb"; // Manufacturer Name
charConfig.properties = static_cast<uint32_t>(CharacteristicProperty::Read);
charConfig.permissions = static_cast<uint32_t>(CharacteristicPermission::Read);
charConfig.initialValue = {'M', 'y', 'C', 'o', 'm', 'p', 'a', 'n', 'y'};
server->AddCharacteristic(serviceConfig.uuid, charConfig);

// Set read callback
server->SetCharacteristicReadCallback(
    serviceConfig.uuid,
    charConfig.uuid,
    [](const std::string& deviceId) {
        return std::vector<uint8_t>{'H', 'e', 'l', 'l', 'o'};
    }
);

// Start advertising
server->StartAdvertising();

// Keep running...
std::this_thread::sleep_for(std::chrono::hours(1));

// Cleanup
server->Shutdown();
```

### Basic Usage (C API for Unity)

```c
#include "IBleServer.h"

// Create server
void* server = BleServer_Create();

// Initialize
BleServer_Initialize(server, "My BLE Device", false);

// Add service
BleServer_AddService(server, "0000180a-0000-1000-8000-00805f9b34fb", true);

// Add characteristic
BleServer_AddCharacteristic(
    server,
    "0000180a-0000-1000-8000-00805f9b34fb",
    "00002a29-0000-1000-8000-00805f9b34fb",
    0x02, // Read property
    0x01  // Read permission
);

// Start advertising
BleServer_StartAdvertising(server);

// Update value
uint8_t data[] = {0x48, 0x65, 0x6C, 0x6C, 0x6F}; // "Hello"
BleServer_UpdateCharacteristicValue(
    server,
    "0000180a-0000-1000-8000-00805f9b34fb",
    "00002a29-0000-1000-8000-00805f9b34fb",
    data,
    5
);

// Notify clients
BleServer_NotifyCharacteristicChanged(
    server,
    "0000180a-0000-1000-8000-00805f9b34fb",
    "00002a29-0000-1000-8000-00805f9b34fb",
    NULL // NULL = notify all connected devices
);

// Cleanup
BleServer_Destroy(server);
```

## API Reference

### Core Classes

#### `IBleServer`

Main interface for BLE server operations.

**Lifecycle Methods:**
- `BleError Initialize(const ServerConfig& config)` - Initialize the server
- `void Shutdown()` - Shutdown and cleanup
- `bool IsInitialized() const` - Check initialization status

**Service Management:**
- `BleError AddService(const ServiceConfig& config)` - Add a GATT service
- `BleError RemoveService(const std::string& uuid)` - Remove a service

**Characteristic Management:**
- `BleError AddCharacteristic(const std::string& serviceUuid, const CharacteristicConfig& config)` - Add a characteristic
- `BleError RemoveCharacteristic(const std::string& serviceUuid, const std::string& charUuid)` - Remove a characteristic

**Callbacks:**
- `BleError SetCharacteristicReadCallback(...)` - Set read handler
- `BleError SetCharacteristicWriteCallback(...)` - Set write handler
- `void SetConnectionCallback(ConnectionCallback)` - Set connection handler

**Advertising:**
- `BleError StartAdvertising()` - Start LE advertising
- `BleError StopAdvertising()` - Stop advertising
- `bool IsAdvertising() const` - Check advertising status

**Data Operations:**
- `BleError UpdateCharacteristicValue(...)` - Update characteristic value
- `BleError NotifyCharacteristicChanged(...)` - Send notification to clients

#### `BleMessageChunker`

Utility for splitting large messages into BLE-compatible chunks.

**Constants:**
- `CHUNK_SIZE = 256` - Total chunk size
- `HEADER_SIZE = 13` - Header overhead (type + messageId + chunkIndex + totalChunks)
- `MAX_PAYLOAD_SIZE = 243` - Maximum payload per chunk

**Methods:**
- `static std::vector<std::vector<uint8_t>> CreateChunks(const std::vector<uint8_t>& message)` - Split message
- `static bool ProcessChunk(...)` - Reassemble received chunks
- `static std::vector<uint8_t> CreateAck(uint32_t messageId, uint32_t chunkIndex)` - Create acknowledgment

### Configuration Structures

```cpp
struct ServerConfig {
    std::string deviceName = "BLE Server";
    bool requiresPairing = false;
    bool requiresBonding = false;
    int maxConnections = 10;
};

struct ServiceConfig {
    std::string uuid;
    bool isPrimary = true;
};

struct CharacteristicConfig {
    std::string uuid;
    uint32_t properties = 0;      // Bitwise OR of CharacteristicProperty flags
    uint32_t permissions = 0;     // Bitwise OR of CharacteristicPermission flags
    std::vector<uint8_t> initialValue;
    size_t maxLength = 512;
};
```

### Enums

```cpp
enum class CharacteristicProperty {
    Read = 0x02,
    Write = 0x08,
    Notify = 0x10,
    Indicate = 0x20,
    WriteWithoutResponse = 0x04
};

enum class CharacteristicPermission {
    Read = 0x01,
    Write = 0x02,
    ReadEncrypted = 0x04,
    WriteEncrypted = 0x08
};

enum class BleError {
    Success = 0,
    NotSupported,
    NotInitialized,
    AlreadyInitialized,
    InvalidParameter,
    ServiceNotFound,
    CharacteristicNotFound,
    BluetoothDisabled,
    AdvertisingFailed,
    Unknown
};
```

## Unity Integration

### 1. Build the Library

Build the library for your target platform (see Building section above).

### 2. Copy to Unity Assets

Copy the built library to your Unity project:
- **Windows**: `BleServer.dll` → `Assets/Plugins/Windows/x86_64/`
- **Linux**: `libBleServer.so` → `Assets/Plugins/Linux/x86_64/`

### 3. Create C# Wrapper

```csharp
using System;
using System.Runtime.InteropServices;
using UnityEngine;

public class BleServerWrapper : MonoBehaviour
{
    private IntPtr server;

    [DllImport("BleServer")]
    private static extern IntPtr BleServer_Create();

    [DllImport("BleServer")]
    private static extern int BleServer_Initialize(IntPtr server, string deviceName, bool requiresPairing);

    [DllImport("BleServer")]
    private static extern int BleServer_AddService(IntPtr server, string serviceUuid, bool isPrimary);

    [DllImport("BleServer")]
    private static extern int BleServer_StartAdvertising(IntPtr server);

    [DllImport("BleServer")]
    private static extern void BleServer_Destroy(IntPtr server);

    void Start()
    {
        server = BleServer_Create();
        BleServer_Initialize(server, "Unity BLE Server", false);
        BleServer_AddService(server, "0000180a-0000-1000-8000-00805f9b34fb", true);
        BleServer_StartAdvertising(server);
    }

    void OnApplicationQuit()
    {
        if (server != IntPtr.Zero)
        {
            BleServer_Destroy(server);
        }
    }
}
```

## Common BLE Service UUIDs

| Service | UUID |
|---------|------|
| Device Information | `0000180a-0000-1000-8000-00805f9b34fb` |
| Battery Service | `0000180f-0000-1000-8000-00805f9b34fb` |
| Heart Rate | `0000180d-0000-1000-8000-00805f9b34fb` |
| Generic Access | `00001800-0000-1000-8000-00805f9b34fb` |
| Generic Attribute | `00001801-0000-1000-8000-00805f9b34fb` |

See [Bluetooth SIG Assigned Numbers](https://www.bluetooth.com/specifications/assigned-numbers/) for more.

## Troubleshooting

### Windows

**Issue: "Bluetooth adapter not found"**
- Ensure Bluetooth is enabled in Windows Settings
- Check that your device has BLE capability (not just classic Bluetooth)

**Issue: "Failed to create service provider"**
- UUID format must be valid (either XXXX or XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX)
- Ensure application has Bluetooth capabilities (see app manifest)

### Linux

**Issue: "No Bluetooth adapter found"**
```bash
# Check if BlueZ is running
sudo systemctl status bluetooth

# Start if needed
sudo systemctl start bluetooth
```

**Issue: "Failed to register GATT application"**
```bash
# Check BlueZ version (need 5.x)
bluetoothctl --version

# Monitor D-Bus for errors
dbus-monitor --system "sender='org.bluez'"
```

**Issue: "Permission denied"**
```bash
# Add user to bluetooth group
sudo usermod -a -G bluetooth $USER
# Log out and back in
```

## Testing

### Using `bluetoothctl` (Linux)

```bash
bluetoothctl
> scan on
> devices
> connect XX:XX:XX:XX:XX:XX
> menu gatt
> list-attributes
> select-attribute /org/bluez/...
> read
> write 0x48656c6c6f  # "Hello"
```

### Using Windows Settings

1. Open Settings → Devices → Bluetooth & other devices
2. Click "Add Bluetooth or other device"
3. Select your BLE server from the list
4. Use a BLE scanner app to explore services/characteristics

### Using nRF Connect Mobile App

Available for iOS and Android. Provides a full GATT explorer.

## Architecture

```
┌─────────────────────────────────────────┐
│    C API (Unity/C# Integration)         │
└─────────────────────────────────────────┘
              │
┌─────────────────────────────────────────┐
│   C++ Interface (IBleServer)            │
│   - Service/Characteristic Management   │
│   - Callbacks & Event Handling          │
│   - Input Validation (BleUtilities)     │
└─────────────────────────────────────────┘
              │
┌─────────────────────────────────────────┐
│  Common Code                            │
│  - BleMessageChunker                    │
│  - BleUtilities (validators)            │
└─────────────────────────────────────────┘
              │
      ┌───────────────┐
      │               │
┌─────▼──────┐  ┌────▼────────┐
│  Windows   │  │   Linux     │
│  (WinRT)   │  │  (BlueZ)    │
│            │  │  (D-Bus)    │
└────────────┘  └─────────────┘
```

## Performance

- **Max Connections**: Configurable 1-10 simultaneous connections
- **Characteristic Size**: Up to 512 bytes per characteristic (configurable)
- **Message Chunking**: Automatic for messages >243 bytes
- **Throughput**: Depends on BLE MTU and connection interval (typically 1-2 KB/s)

## Security

- **Pairing**: Configurable via `ServerConfig.requiresPairing`
- **Bonding**: Configurable via `ServerConfig.requiresBonding`
- **Encryption**: Supported via `CharacteristicPermission::ReadEncrypted` / `WriteEncrypted`
- **Validation**: All inputs validated (UUIDs, values, parameters)

## Known Limitations

1. **Windows**: Cannot forcefully disconnect clients (BLE peripheral limitation)
2. **Windows**: RemoveCharacteristic unregisters handlers but doesn't remove from GATT
3. **Linux**: Requires BlueZ 5.x (not compatible with BlueZ 4.x)
4. **Both**: No support for characteristic descriptors yet (planned)

## Contributing

Contributions are welcome! Please ensure:
- Code follows existing style
- All platforms build successfully
- Input validation is comprehensive
- Thread safety is maintained
- Documentation is updated


## Credits

Developed for cross-platform BLE GATT server functionality with Unity integration.

## Support

For issues, questions, or feature requests, please open an issue on GitHub.
