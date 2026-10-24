# BLE Server Linux Implementation - Developer Quick Reference

## File Location
`src/platforms/linux/BleServerLinux.cpp`

## Key Implementation Components

### 1. D-Bus Introspection XML (Lines 28-132)
Pre-defined XML schemas for all BlueZ interfaces:
- `GATT_SERVICE_INTROSPECTION` - GATT Service interface
- `GATT_CHARACTERISTIC_INTROSPECTION` - GATT Characteristic with ReadValue, WriteValue, StartNotify, StopNotify
- `LE_ADVERTISEMENT_INTROSPECTION` - LE Advertisement interface
- `APPLICATION_INTROSPECTION` - ObjectManager for GATT hierarchy

### 2. Core Data Structures (Lines 136-186)

**CharacteristicData:**
```cpp
struct CharacteristicData {
    CharacteristicConfig config;           // UUID, properties, permissions
    CharacteristicReadCallback readCallback;
    CharacteristicWriteCallback writeCallback;
    std::vector<uint8_t> value;            // Current value
    std::mutex valueMutex;                 // Thread-safe access
    GDBusNodeInfo* nodeInfo;               // D-Bus introspection
    guint registrationId;                  // D-Bus object ID
    std::string objectPath;                // D-Bus path
    bool notifying;                        // Notification state
    std::set<std::string> subscribedDevices; // Clients subscribed to notifications
};
```

**ServiceData:**
```cpp
struct ServiceData {
    ServiceConfig config;                  // UUID, isPrimary
    std::map<std::string, CharacteristicData> characteristics;
    GDBusNodeInfo* nodeInfo;
    guint registrationId;
    std::string objectPath;
};
```

### 3. Public API Methods

#### Initialize() - Lines 204-231
- Connects to D-Bus system bus
- Finds Bluetooth adapter
- Sets up device monitoring
- Starts GLib main loop in separate thread

#### AddService() - Lines 297-318
- Validates service doesn't exist
- Calls RegisterService() for D-Bus registration
- Stores service in m_services map

#### AddCharacteristic() - Lines 343-370
- Validates parent service exists
- Calls RegisterCharacteristic() for D-Bus registration
- Stores characteristic under parent service

#### StartAdvertising() - Lines 429-447
- Calls CreateAdvertisement()
- Registers with BlueZ LEAdvertisingManager1
- Sets m_advertising flag

#### NotifyCharacteristicChanged() - Lines 500-513
- Updates characteristic value (optional)
- Calls EmitPropertiesChanged()
- Broadcasts to all subscribed clients

### 4. Core Implementation Methods

#### RegisterService() - Lines 675-738
**Purpose:** Register GATT service with BlueZ via D-Bus

**Steps:**
1. Generate unique object path: `/com/example/ble/service<index>`
2. Parse GATT_SERVICE_INTROSPECTION XML
3. Create vtable with callbacks:
   - OnServiceMethodCall
   - OnServiceGetProperty
4. Register with g_dbus_connection_register_object()
5. Register Properties interface

**D-Bus Properties:**
- `UUID` - Service UUID string
- `Primary` - Boolean (primary or secondary service)

#### RegisterCharacteristic() - Lines 767-838
**Purpose:** Register GATT characteristic with proper D-Bus object path

**Steps:**
1. Generate object path: `<service-path>/char<uuid>`
2. Parse GATT_CHARACTERISTIC_INTROSPECTION XML
3. Create vtable with callbacks:
   - OnCharacteristicMethodCall (ReadValue, WriteValue, StartNotify, StopNotify)
   - OnCharacteristicGetProperty
4. Register with g_dbus_connection_register_object()

**D-Bus Properties:**
- `UUID` - Characteristic UUID
- `Service` - Parent service object path
- `Value` - Byte array (current value)
- `Notifying` - Boolean (notification enabled)
- `Flags` - String array (["read", "write", "notify", ...])

**D-Bus Methods:**
- `ReadValue(options: a{sv}) -> ay` - Read characteristic value
- `WriteValue(value: ay, options: a{sv})` - Write characteristic value
- `StartNotify()` - Enable notifications
- `StopNotify()` - Disable notifications

#### CreateAdvertisement() - Lines 944-1037
**Purpose:** Create and register LE advertisement

**Steps:**
1. Parse LE_ADVERTISEMENT_INTROSPECTION XML
2. Create vtable with callbacks:
   - OnAdvertisementMethodCall (Release)
   - OnAdvertisementGetProperty
3. Register object at `/com/example/ble/advertisement`
4. Call `LEAdvertisingManager1.RegisterAdvertisement()`
5. Auto-register GATT application if needed

**D-Bus Properties:**
- `Type` - "peripheral"
- `ServiceUUIDs` - Array of service UUIDs to advertise
- `LocalName` - Device name
- `Includes` - ["tx-power"]

#### EmitPropertiesChanged() - Lines 1078-1122
**Purpose:** Send notification signal when value changes

**Steps:**
1. Build GVariant with new value
2. Emit PropertiesChanged signal:
   ```
   PropertiesChanged(
     "org.bluez.GattCharacteristic1",
     {"Value": <byte-array>},
     []
   )
   ```
3. Broadcast to all connected clients

### 5. D-Bus Callback Handlers

#### OnCharacteristicMethodCall() - Lines 1223-1322
**Handles incoming D-Bus method calls on characteristic**

**ReadValue:**
```cpp
// Extracts device ID from options
// Calls readCallback if set
// Returns byte array variant
```

**WriteValue:**
```cpp
// Extracts byte array from parameters
// Updates internal value
// Calls writeCallback if set
```

**StartNotify:**
```cpp
// Adds sender to subscribedDevices
// Sets notifying = true
```

**StopNotify:**
```cpp
// Removes sender from subscribedDevices
// Sets notifying = false if no subscribers
```

#### OnApplicationMethodCall() - Lines 1389-1478
**Handles GetManagedObjects - returns complete GATT tree**

Returns nested dictionary:
```
a{oa{sa{sv}}}  // object-path -> interface -> properties
```

This is called by BlueZ when the application is registered to enumerate all services and characteristics.

### 6. Device Connection Tracking - Lines 615-673

**SetupDeviceMonitoring():**
- Subscribes to PropertiesChanged on org.bluez.Device1
- Monitors "Connected" property changes

**OnDevicePropertiesChanged():**
- Static callback for D-Bus signal
- Extracts Connected property
- Calls HandleDeviceConnection()

**HandleDeviceConnection():**
- Updates m_connectedDevices set
- Invokes m_connectionCallback

### 7. Resource Management

**Cleanup in Shutdown():**
```cpp
1. StopAdvertising() -> RemoveAdvertisement()
2. UnregisterApplication()
3. UnregisterService() for all services
4. Unsubscribe from device signals
5. Stop GLib main loop
6. Free GDBusNodeInfo objects
7. Unref D-Bus connection
```

**Important:** All GDBusNodeInfo must be freed with g_dbus_node_info_unref()

### 8. Thread Safety

**Mutexes:**
- `m_mutex` - Protects all service/characteristic maps and state
- `CharacteristicData::valueMutex` - Protects value vector

**Thread Model:**
- Main thread: Public API calls
- D-Bus thread: Callback handlers (OnCharacteristicMethodCall, etc.)
- GLib thread: Main loop (m_dbusThread)

**Locking Strategy:**
- Always lock m_mutex for map access
- Lock valueMutex only when accessing value
- Don't hold multiple locks simultaneously (deadlock prevention)

### 9. Error Reporting

**ReportError() - Lines 1516-1520**
```cpp
void ReportError(BleError error, const std::string& message) {
    if (m_errorCallback) {
        m_errorCallback(error, message);
    }
}
```

**Usage:**
```cpp
if (error) {
    ReportError(BleError::Unknown, "Failed to register: " + error->message);
    g_error_free(error);
    return false;
}
```

### 10. Helper Functions

#### UuidToPath() - Lines 540-545
Converts UUID string to valid D-Bus path component:
```cpp
"0000180a-0000-1000-8000-00805f9b34fb"
-> "0000180a_0000_1000_8000_00805f9b34fb"
```

#### GetCharacteristicFlags() - Lines 548-568
Converts property bitmask to BlueZ flag strings:
```cpp
CharacteristicProperty::Read | CharacteristicProperty::Notify
-> ["read", "notify"]
```

## Usage Example

```cpp
// Initialize
BleServerLinux server;
ServerConfig config;
config.deviceName = "My BLE Device";
server.Initialize(config);

// Add service
ServiceConfig svcConfig;
svcConfig.uuid = "0000180a-0000-1000-8000-00805f9b34fb";
svcConfig.isPrimary = true;
server.AddService(svcConfig);

// Add characteristic
CharacteristicConfig charConfig;
charConfig.uuid = "00002a29-0000-1000-8000-00805f9b34fb";
charConfig.properties =
    static_cast<uint32_t>(CharacteristicProperty::Read) |
    static_cast<uint32_t>(CharacteristicProperty::Notify);
charConfig.initialValue = {'H', 'e', 'l', 'l', 'o'};
server.AddCharacteristic(svcConfig.uuid, charConfig);

// Set callbacks
server.SetCharacteristicReadCallback(
    svcConfig.uuid, charConfig.uuid,
    [](const std::string& deviceId) -> std::vector<uint8_t> {
        return {'W', 'o', 'r', 'l', 'd'};
    }
);

server.SetCharacteristicWriteCallback(
    svcConfig.uuid, charConfig.uuid,
    [](const std::string& deviceId, const std::vector<uint8_t>& data) {
        // Handle write
    }
);

// Start advertising
server.StartAdvertising();

// Update and notify
std::vector<uint8_t> newValue = {'B', 'y', 'e'};
server.UpdateCharacteristicValue(svcConfig.uuid, charConfig.uuid, newValue);
server.NotifyCharacteristicChanged(svcConfig.uuid, charConfig.uuid);

// Cleanup
server.Shutdown();
```

## Debugging Tips

### Monitor D-Bus Traffic
```bash
# System-wide monitoring
dbus-monitor --system

# Filter BlueZ only
dbus-monitor --system "sender='org.bluez'"

# Monitor specific interface
dbus-monitor --system "interface='org.bluez.GattCharacteristic1'"
```

### Check BlueZ Status
```bash
# Check adapter
bluetoothctl show

# List GATT services (from Linux perspective)
sudo busctl tree org.bluez

# Get service properties
sudo busctl introspect org.bluez /org/bluez/hci0/dev_XX_XX_XX_XX_XX_XX/service0000

# Check advertising
sudo btmon
```

### Common Issues

**Issue:** Services not visible to BLE clients
- **Check:** Application registered? Call RegisterApplication()
- **Check:** Advertisement active? Call CreateAdvertisement()
- **Check:** BlueZ adapter powered? `bluetoothctl power on`

**Issue:** ReadValue/WriteValue not called
- **Check:** Characteristic flags correct? Must include "read"/"write"
- **Check:** D-Bus object registered? Check registrationId != 0
- **Check:** Permissions? May need root or bluetooth group

**Issue:** Notifications not working
- **Check:** Flags include "notify"?
- **Check:** Client subscribed? StartNotify called?
- **Check:** EmitPropertiesChanged() being called?

## Build Instructions

```bash
# Install dependencies
sudo apt-get install libbluetooth-dev libglib2.0-dev libdbus-1-dev

# Compile
g++ -std=c++17 -c src/platforms/linux/BleServerLinux.cpp \
    -I./include \
    `pkg-config --cflags glib-2.0 gio-2.0` \
    -o BleServerLinux.o

# Link
g++ BleServerLinux.o ... \
    `pkg-config --libs glib-2.0 gio-2.0` \
    -o ble_server
```

## Testing Checklist

- [ ] Service registration succeeds
- [ ] Characteristic registration succeeds
- [ ] Advertisement starts without errors
- [ ] Visible to BLE scanner (nRF Connect, bluetoothctl)
- [ ] Can connect from BLE client
- [ ] Service discovery shows correct UUIDs
- [ ] Characteristic read returns correct value
- [ ] Characteristic write updates value
- [ ] Write callback invoked
- [ ] Notifications received by client
- [ ] Multiple connections handled
- [ ] Graceful shutdown without leaks
- [ ] No D-Bus errors in logs

## Performance Considerations

- **D-Bus Overhead:** Each characteristic operation involves D-Bus IPC
- **Notification Latency:** ~1-10ms typical for PropertiesChanged emission
- **Connection Limit:** BlueZ typically supports 3-7 simultaneous connections
- **MTU Size:** Default 23 bytes, negotiate up to 512 bytes for better throughput

## Security Notes

- Runs on system D-Bus (requires appropriate permissions)
- Encrypted characteristic flags are mapped to BlueZ `encrypt-read` / `encrypt-write`
- Pairing/bonding policy still depends on BlueZ agent configuration
- Consider running as bluetooth group user, not root
- For stricter production pairing UX: provide/register a BlueZ Agent API implementation

---

**Quick Reference Version:** 1.0
**Last Updated:** 2025-11-16
**BlueZ Version:** 5.x
**GLib Version:** 2.0+
