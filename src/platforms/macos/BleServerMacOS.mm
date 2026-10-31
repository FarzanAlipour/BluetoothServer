// BleServerMacOS.mm - macOS implementation using CoreBluetooth peripheral mode
#include "IBleServer.h"
#include "BleUtilities.h"
#include "NotificationQueue.h"
#include "LongWriteAssembler.h"

#import <Foundation/Foundation.h>
#import <CoreBluetooth/CoreBluetooth.h>

#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace BleServer {
    class BleServerMacOS;
}

@interface BlePeripheralDelegate : NSObject <CBPeripheralManagerDelegate>
- (instancetype)initWithOwner:(BleServer::BleServerMacOS*)owner;
@end

namespace BleServer {

    static NSString* ToNSString(const std::string& value) {
        return [NSString stringWithUTF8String:value.c_str()];
    }

    static std::string ToStdString(NSString* value) {
        return value ? std::string([value UTF8String]) : std::string();
    }

    static CBAttributePermissions ToAttributePermissions(uint32_t permissions) {
        CBAttributePermissions result = 0;
        if (permissions & static_cast<uint32_t>(CharacteristicPermission::Read)) {
            result |= CBAttributePermissionsReadable;
        }
        if (permissions & static_cast<uint32_t>(CharacteristicPermission::Write)) {
            result |= CBAttributePermissionsWriteable;
        }
        if (permissions & static_cast<uint32_t>(CharacteristicPermission::ReadEncrypted)) {
            result |= CBAttributePermissionsReadEncryptionRequired;
        }
        if (permissions & static_cast<uint32_t>(CharacteristicPermission::WriteEncrypted)) {
            result |= CBAttributePermissionsWriteEncryptionRequired;
        }
        return result;
    }

    static CBCharacteristicProperties ToCharacteristicProperties(uint32_t properties) {
        CBCharacteristicProperties result = 0;
        if (properties & static_cast<uint32_t>(CharacteristicProperty::Read)) {
            result |= CBCharacteristicPropertyRead;
        }
        if (properties & static_cast<uint32_t>(CharacteristicProperty::Write)) {
            result |= CBCharacteristicPropertyWrite;
        }
        if (properties & static_cast<uint32_t>(CharacteristicProperty::WriteWithoutResponse)) {
            result |= CBCharacteristicPropertyWriteWithoutResponse;
        }
        if (properties & static_cast<uint32_t>(CharacteristicProperty::Notify)) {
            result |= CBCharacteristicPropertyNotify;
        }
        if (properties & static_cast<uint32_t>(CharacteristicProperty::Indicate)) {
            result |= CBCharacteristicPropertyIndicate;
        }
        return result;
    }

    class BleServerMacOS : public IBleServer {
    private:
        struct CharacteristicData {
            CBMutableCharacteristic* characteristic = nil;
            CharacteristicConfig config;
            CharacteristicReadCallback readCallback;
            CharacteristicWriteCallback writeCallback;
            std::vector<uint8_t> value;
            std::mutex valueMutex;
            std::set<std::string> subscribedDevices;
        };

        struct ServiceData {
            CBMutableService* service = nil;
            ServiceConfig config;
            std::map<std::string, std::unique_ptr<CharacteristicData>> characteristics;
        };

        ServerConfig m_config;
        bool m_initialized = false;
        bool m_advertising = false;
        bool m_advertisingRequested = false;
        size_t m_pendingServiceAdds = 0;
        std::map<std::string, std::unique_ptr<ServiceData>> m_services;
        std::map<CBUUID*, CharacteristicData*> m_characteristicsByUuid;
        std::set<std::string> m_connectedDevices;
        ConnectionCallback m_connectionCallback;
        ErrorCallback m_errorCallback;
        mutable std::mutex m_mutex;

        // Notifications CoreBluetooth could not take yet (updateValue returned NO). Guarded by
        // m_mutex; drained from peripheralManagerIsReadyToUpdateSubscribers:.
        NotificationQueue m_notifyQueue;

        // Messages collected while m_mutex is held and delivered after it is released
        // (no user callback under a lock).
        using Reports = std::vector<std::pair<BleError, std::string>>;

        BlePeripheralDelegate* m_delegate = nil;
        CBPeripheralManager* m_peripheral = nil;
        dispatch_queue_t m_queue = nullptr;

    public:
        ~BleServerMacOS() override {
            Shutdown();
        }

        BleError Initialize(const ServerConfig& config) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_initialized) {
                return BleError::AlreadyInitialized;
            }
            if (!Utils::IsValidDeviceName(config.deviceName) || !Utils::IsValidMaxConnections(config.maxConnections)) {
                return BleError::InvalidParameter;
            }

            m_config = config;
            m_delegate = [[BlePeripheralDelegate alloc] initWithOwner:this];
            // Use a dedicated serial queue so CoreBluetooth callbacks fire
            // independently of the Unity main thread runloop.
            m_queue = dispatch_queue_create("com.bluetoothserver.ble", DISPATCH_QUEUE_SERIAL);
            m_peripheral = [[CBPeripheralManager alloc] initWithDelegate:m_delegate queue:m_queue];
            m_initialized = true;
            NSLog(@"[BluetoothServer] Initialize: device='%s' max=%d pairing=%d bonding=%d",
                config.deviceName.c_str(), config.maxConnections, config.requiresPairing, config.requiresBonding);
            return BleError::Success;
        }

        void Shutdown() override {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_initialized) {
                return;
            }

            if (m_peripheral) {
                [m_peripheral stopAdvertising];
                [m_peripheral removeAllServices];
            }
            if (const size_t dropped = m_notifyQueue.Clear()) {
                NSLog(@"[BluetoothServer] Shutdown: dropped %zu queued notification(s)", dropped);
            }
            m_services.clear();
            m_characteristicsByUuid.clear();
            m_connectedDevices.clear();
            m_advertising = false;
            m_advertisingRequested = false;
            m_pendingServiceAdds = 0;
            m_initialized = false;
            m_peripheral = nil;
            m_delegate = nil;
            m_queue = nullptr;
        }

        bool IsInitialized() const override {
            return m_initialized;
        }

        BleError AddService(const ServiceConfig& config) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_initialized) {
                return BleError::NotInitialized;
            }
            if (!Utils::IsValidUuid(config.uuid)) {
                return BleError::InvalidParameter;
            }
            if (m_services.find(config.uuid) != m_services.end()) {
                return BleError::InvalidParameter;
            }

            auto serviceData = std::make_unique<ServiceData>();
            serviceData->config = config;
            serviceData->service = [[CBMutableService alloc] initWithType:[CBUUID UUIDWithString:ToNSString(config.uuid)] primary:config.isPrimary];
            m_services[config.uuid] = std::move(serviceData);
            return BleError::Success;
        }

        BleError RemoveService(const std::string& serviceUuid) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_services.find(serviceUuid);
            if (it == m_services.end()) {
                return BleError::ServiceNotFound;
            }
            if (m_peripheral && it->second->service) {
                [m_peripheral removeService:it->second->service];
            }
            m_notifyQueue.RemoveService(serviceUuid);
            m_services.erase(it);
            RebuildCharacteristicIndex();
            return BleError::Success;
        }

        std::vector<std::string> GetServices() const override {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::vector<std::string> services;
            for (const auto& [uuid, service] : m_services) {
                services.push_back(uuid);
            }
            return services;
        }

        BleError AddCharacteristic(const std::string& serviceUuid, const CharacteristicConfig& config) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto serviceIt = m_services.find(serviceUuid);
            if (serviceIt == m_services.end()) {
                return BleError::ServiceNotFound;
            }
            if (!Utils::IsValidUuid(config.uuid) || !Utils::IsValidProperties(config.properties) || !Utils::IsValidPermissions(config.permissions)) {
                return BleError::InvalidParameter;
            }
            auto& service = serviceIt->second;
            if (service->characteristics.find(config.uuid) != service->characteristics.end()) {
                return BleError::InvalidParameter;
            }

            auto charData = std::make_unique<CharacteristicData>();
            charData->config = config;
            charData->value = config.initialValue;
            // CoreBluetooth treats a non-nil local characteristic value as a
            // static cached value. Dynamic read/write/notify characteristics
            // must keep the CB value nil and serve data from delegate methods.
            charData->characteristic = [[CBMutableCharacteristic alloc]
                initWithType:[CBUUID UUIDWithString:ToNSString(config.uuid)]
                properties:ToCharacteristicProperties(config.properties)
                value:nil
                permissions:ToAttributePermissions(config.permissions)];

            service->characteristics[config.uuid] = std::move(charData);
            RebuildServiceCharacteristics(service.get());
            RebuildCharacteristicIndex();
            return BleError::Success;
        }

        BleError RemoveCharacteristic(const std::string& serviceUuid, const std::string& characteristicUuid) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto serviceIt = m_services.find(serviceUuid);
            if (serviceIt == m_services.end()) {
                return BleError::ServiceNotFound;
            }
            auto& service = serviceIt->second;
            auto charIt = service->characteristics.find(characteristicUuid);
            if (charIt == service->characteristics.end()) {
                return BleError::CharacteristicNotFound;
            }
            m_notifyQueue.RemoveCharacteristic(serviceUuid, characteristicUuid);
            service->characteristics.erase(charIt);
            RebuildServiceCharacteristics(service.get());
            RebuildCharacteristicIndex();
            return BleError::Success;
        }

        BleError SetCharacteristicReadCallback(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            CharacteristicReadCallback callback) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto* charData = GetCharacteristicData(serviceUuid, characteristicUuid);
            if (!charData) {
                return BleError::CharacteristicNotFound;
            }
            charData->readCallback = callback;
            return BleError::Success;
        }

        BleError SetCharacteristicWriteCallback(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            CharacteristicWriteCallback callback) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto* charData = GetCharacteristicData(serviceUuid, characteristicUuid);
            if (!charData) {
                return BleError::CharacteristicNotFound;
            }
            charData->writeCallback = callback;
            return BleError::Success;
        }

        BleError StartAdvertising() override {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_initialized || !m_peripheral) {
                return BleError::NotInitialized;
            }
            if (m_peripheral.state != CBManagerStatePoweredOn) {
                return BleError::BluetoothDisabled;
            }

            @try {
                [m_peripheral stopAdvertising];
                [m_peripheral removeAllServices];
                m_advertising = false;
                m_advertisingRequested = true;
                m_pendingServiceAdds = m_services.size();

                if (m_pendingServiceAdds == 0) {
                    StartNativeAdvertisingLocked();
                    return BleError::Success;
                }

                for (auto& [uuid, service] : m_services) {
                    RebuildServiceCharacteristics(service.get());
                    [m_peripheral addService:service->service];
                }
                return BleError::Success;
            }
            @catch (NSException* exception) {
                ReportError(BleError::AdvertisingFailed, "CoreBluetooth advertising exception: " + ToStdString(exception.reason));
                return BleError::AdvertisingFailed;
            }
        }

        BleError StopAdvertising() override {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_peripheral) {
                [m_peripheral stopAdvertising];
            }
            m_advertising = false;
            m_advertisingRequested = false;
            m_pendingServiceAdds = 0;
            return BleError::Success;
        }

        bool IsAdvertising() const override {
            return m_peripheral ? m_peripheral.isAdvertising : m_advertising;
        }

        void SetConnectionCallback(ConnectionCallback callback) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_connectionCallback = callback;
        }

        std::vector<std::string> GetConnectedDevices() const override {
            std::lock_guard<std::mutex> lock(m_mutex);
            return std::vector<std::string>(m_connectedDevices.begin(), m_connectedDevices.end());
        }

        BleError DisconnectDevice(const std::string& deviceId) override {
            (void)deviceId;
            return BleError::NotSupported;
        }

        BleError UpdateCharacteristicValue(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            const std::vector<uint8_t>& value) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto* charData = GetCharacteristicData(serviceUuid, characteristicUuid);
            if (!charData) {
                return BleError::CharacteristicNotFound;
            }
            std::lock_guard<std::mutex> valueLock(charData->valueMutex);
            charData->value = value;
            return BleError::Success;
        }

        BleError NotifyCharacteristicChanged(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            const std::string& deviceId) override {
            // Flow control (see IBleServer::NotifyCharacteristicChanged): the current value is
            // snapshotted now; if CoreBluetooth's transmit queue is full it is queued and sent from
            // peripheralManagerIsReadyToUpdateSubscribers:, in order. Success means sent or queued.
            (void)deviceId;
            Reports reports;
            ErrorCallback errorCallback;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_initialized || !m_peripheral) {
                    return BleError::NotInitialized;
                }
                auto* charData = GetCharacteristicData(serviceUuid, characteristicUuid);
                if (!charData) {
                    return BleError::CharacteristicNotFound;
                }
                NotificationQueue::Item item{serviceUuid, characteristicUuid, {}};
                {
                    std::lock_guard<std::mutex> valueLock(charData->valueMutex);
                    item.value = charData->value;
                }
                const auto r = m_notifyQueue.Submit(std::move(item),
                    [this](const NotificationQueue::Item& it) { return SendNotificationLocked(it); });
                if (r.backlogStarted) {
                    NSLog(@"[BluetoothServer] notify queue backed up: CoreBluetooth transmit queue full on %s, "
                          "queued (depth %zu)", characteristicUuid.c_str(), r.depth);
                    reports.emplace_back(BleError::Success,
                        "Notification queue backed up: CoreBluetooth transmit queue full on " + characteristicUuid +
                        ", queued (depth " + std::to_string(r.depth) + ")");
                }
                if (r.dropped > 0) {
                    NSLog(@"[BluetoothServer] notify queue full (%zu): dropped %zu oldest notification(s)",
                        m_notifyQueue.Capacity(), r.dropped);
                    reports.emplace_back(BleError::Unknown,
                        "Notification queue full (" + std::to_string(m_notifyQueue.Capacity()) + "): dropped " +
                        std::to_string(r.dropped) + " oldest notification(s)");
                }
                errorCallback = m_errorCallback;
            }
            Deliver(errorCallback, reports);
            return BleError::Success;
        }

        void SetErrorCallback(ErrorCallback callback) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_errorCallback = callback;
        }

        uint32_t GetCapabilities() const override {
            return static_cast<uint32_t>(ServerCapability::Advertising) |
                static_cast<uint32_t>(ServerCapability::Connections) |
                static_cast<uint32_t>(ServerCapability::Read) |
                static_cast<uint32_t>(ServerCapability::Write) |
                static_cast<uint32_t>(ServerCapability::Notify) |
                static_cast<uint32_t>(ServerCapability::Indicate) |
                static_cast<uint32_t>(ServerCapability::Encryption) |
                static_cast<uint32_t>(ServerCapability::Chunking);
        }

        void HandleStateUpdate() {
            if (!m_peripheral) {
                return;
            }

            const char* stateName = "?";
            switch (m_peripheral.state) {
                case CBManagerStateUnknown:      stateName = "Unknown"; break;
                case CBManagerStateResetting:    stateName = "Resetting"; break;
                case CBManagerStateUnsupported:  stateName = "Unsupported"; break;
                case CBManagerStateUnauthorized: stateName = "Unauthorized"; break;
                case CBManagerStatePoweredOff:   stateName = "PoweredOff"; break;
                case CBManagerStatePoweredOn:    stateName = "PoweredOn"; break;
            }
            NSLog(@"[BluetoothServer] CoreBluetooth state changed: %s", stateName);

            switch (m_peripheral.state) {
                case CBManagerStateUnknown:
                    ReportError(BleError::Unknown, "CoreBluetooth state: Unknown");
                    break;
                case CBManagerStateResetting:
                    ReportError(BleError::Unknown, "CoreBluetooth state: Resetting");
                    break;
                case CBManagerStateUnsupported:
                    ReportError(BleError::NotSupported, "CoreBluetooth state: Unsupported (this Mac cannot act as a BLE peripheral)");
                    break;
                case CBManagerStateUnauthorized:
                    ReportError(BleError::NotSupported, "CoreBluetooth state: Unauthorized. Grant Unity Bluetooth permission in System Settings > Privacy & Security > Bluetooth.");
                    break;
                case CBManagerStatePoweredOff:
                    ReportError(BleError::BluetoothDisabled, "CoreBluetooth state: PoweredOff. Turn Bluetooth on.");
                    break;
                case CBManagerStatePoweredOn:
                    ReportError(BleError::Success, "CoreBluetooth state: PoweredOn");
                    break;
            }
        }

        void HandleServiceAdded(CBService* service, NSError* error) {
            NSLog(@"[BluetoothServer] didAddService: %@ error=%@", service.UUID, error);
            std::lock_guard<std::mutex> lock(m_mutex);
            if (error) {
                m_advertisingRequested = false;
                m_pendingServiceAdds = 0;
                ReportError(BleError::AdvertisingFailed, "CoreBluetooth failed to add service " +
                    ToStdString(service.UUID.UUIDString) + ": " + ToStdString(error.localizedDescription));
                return;
            }

            if (m_pendingServiceAdds > 0) {
                --m_pendingServiceAdds;
            }

            ReportError(BleError::Success, "CoreBluetooth added service " +
                ToStdString(service.UUID.UUIDString) + ", pending services: " + std::to_string(m_pendingServiceAdds));

            if (m_advertisingRequested && m_pendingServiceAdds == 0) {
                StartNativeAdvertisingLocked();
            }
        }

        void HandleAdvertisingStarted(NSError* error) {
            NSLog(@"[BluetoothServer] didStartAdvertising error=%@", error);
            std::lock_guard<std::mutex> lock(m_mutex);
            if (error) {
                m_advertising = false;
                m_advertisingRequested = false;
                ReportError(BleError::AdvertisingFailed, "CoreBluetooth failed to start advertising: " +
                    ToStdString(error.localizedDescription));
                return;
            }

            m_advertising = true;
            ReportError(BleError::Success, "CoreBluetooth advertising started");
        }

        void HandleRead(CBATTRequest* request) {
            NSLog(@"[BluetoothServer] didReceiveReadRequest characteristic=%@ central=%@ offset=%lu",
                request.characteristic.UUID, request.central.identifier, (unsigned long)request.offset);
            std::lock_guard<std::mutex> lock(m_mutex);
            auto* charData = FindCharacteristic(request.characteristic.UUID);
            if (!charData) {
                ReportError(BleError::CharacteristicNotFound, "macOS read request for unknown characteristic " +
                    ToStdString(request.characteristic.UUID.UUIDString));
                [m_peripheral respondToRequest:request withResult:CBATTErrorAttributeNotFound];
                return;
            }

            std::vector<uint8_t> value;
            if (charData->readCallback) {
                value = charData->readCallback(ToStdString(request.central.identifier.UUIDString));
            }
            else {
                std::lock_guard<std::mutex> valueLock(charData->valueMutex);
                value = charData->value;
            }

            if (request.offset > value.size()) {
                ReportError(BleError::InvalidParameter, "macOS read request had invalid offset");
                [m_peripheral respondToRequest:request withResult:CBATTErrorInvalidOffset];
                return;
            }

            NSData* data = [NSData dataWithBytes:value.data() length:value.size()];
            request.value = [data subdataWithRange:NSMakeRange(request.offset, value.size() - request.offset)];
            [m_peripheral respondToRequest:request withResult:CBATTErrorSuccess];
            ReportError(BleError::Success, "macOS read request served " + std::to_string(value.size()) + " bytes from " +
                ToStdString(request.characteristic.UUID.UUIDString));
        }

        static CBATTError ToAttError(LongWrite::Status status) {
            switch (status) {
                case LongWrite::Status::Ok: return CBATTErrorSuccess;
                case LongWrite::Status::InvalidOffset: return CBATTErrorInvalidOffset;
                case LongWrite::Status::InvalidAttributeValueLength: return CBATTErrorInvalidAttributeValueLength;
                case LongWrite::Status::AttributeNotFound: return CBATTErrorAttributeNotFound;
            }
            return CBATTErrorUnlikelyError;
        }

        // One didReceiveWriteRequests: call is one ATT operation: a single Write Request, or a whole
        // Execute Write (Prepare Write fragments with offsets, possibly for several characteristics).
        // Apple: respond exactly once, to the first request; the result applies to all of them.
        // Fragments are assembled per characteristic, all-or-nothing, and each write callback fires
        // once with the full value.
        void HandleWrite(NSArray<CBATTRequest*>* requests) {
            NSLog(@"[BluetoothServer] didReceiveWriteRequests count=%lu", (unsigned long)requests.count);
            CBATTRequest* first = requests.firstObject;
            if (!first) {
                return;
            }
            std::lock_guard<std::mutex> lock(m_mutex);

            std::vector<LongWrite::Fragment> fragments;
            std::map<std::string, CharacteristicData*> targets;
            fragments.reserve(requests.count);
            for (CBATTRequest* request in requests) {
                NSLog(@"[BluetoothServer]   write characteristic=%@ offset=%lu length=%lu",
                    request.characteristic.UUID, (unsigned long)request.offset, (unsigned long)request.value.length);
                LongWrite::Fragment f;
                f.key = ToStdString(request.characteristic.UUID.UUIDString);
                f.offset = request.offset;
                if (request.value.length > 0) {
                    const auto* bytes = static_cast<const uint8_t*>(request.value.bytes);
                    f.data.assign(bytes, bytes + request.value.length);
                }
                if (targets.find(f.key) == targets.end()) {
                    targets[f.key] = FindCharacteristic(request.characteristic.UUID);
                }
                fragments.push_back(std::move(f));
            }

            const auto result = LongWrite::AssembleBatch(fragments,
                [&targets](const std::string& key, std::vector<uint8_t>& out) {
                    out.clear();   // a write operation delivers a new value, starting at offset 0
                    auto it = targets.find(key);
                    return it != targets.end() && it->second != nullptr;
                });

            if (result.status != LongWrite::Status::Ok) {
                const auto& bad = fragments[result.failedIndex];
                [m_peripheral respondToRequest:first withResult:ToAttError(result.status)];
                ReportError(result.status == LongWrite::Status::AttributeNotFound
                        ? BleError::CharacteristicNotFound : BleError::InvalidParameter,
                    std::string("macOS write rejected (") + LongWrite::ToString(result.status) + ") for " + bad.key +
                    " at offset " + std::to_string(bad.offset) + ", " + std::to_string(bad.data.size()) +
                    " bytes; nothing applied");
                return;
            }

            const std::string deviceId = ToStdString(first.central.identifier.UUIDString);
            for (const auto& assembled : result.values) {
                CharacteristicData* charData = targets[assembled.key];
                {
                    std::lock_guard<std::mutex> valueLock(charData->valueMutex);
                    charData->value = assembled.value;
                }
                if (charData->writeCallback) {
                    charData->writeCallback(deviceId, assembled.value);
                }
            }
            [m_peripheral respondToRequest:first withResult:CBATTErrorSuccess];
            for (const auto& assembled : result.values) {
                ReportError(BleError::Success, "macOS write request accepted " + std::to_string(assembled.value.size()) +
                    " bytes to " + assembled.key + " (" + std::to_string(requests.count) + " request(s))");
            }
        }

        void HandleSubscribe(CBCentral* central, CBCharacteristic* characteristic, bool subscribed) {
            NSLog(@"[BluetoothServer] central %@ %@ characteristic %@",
                central.identifier, subscribed ? @"subscribed to" : @"unsubscribed from", characteristic.UUID);
            const std::string deviceId = ToStdString(central.identifier.UUIDString);
            ConnectionCallback connectionCallback;
            ErrorCallback errorCallback;
            Reports reports;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto* charData = FindCharacteristic(characteristic.UUID);
                if (charData) {
                    if (subscribed) {
                        charData->subscribedDevices.insert(deviceId);
                        m_connectedDevices.insert(deviceId);
                    }
                    else {
                        charData->subscribedDevices.erase(deviceId);
                        if (charData->subscribedDevices.empty()) {
                            // Nobody left to receive them: drop this characteristic's queued values.
                            std::string serviceUuid;
                            if (FindServiceUuid(charData, serviceUuid)) {
                                if (const size_t dropped = m_notifyQueue.RemoveCharacteristic(serviceUuid, charData->config.uuid)) {
                                    NSLog(@"[BluetoothServer] last subscriber left %s: dropped %zu queued notification(s)",
                                        charData->config.uuid.c_str(), dropped);
                                    reports.emplace_back(BleError::Success, "Last subscriber left " + charData->config.uuid +
                                        ": dropped " + std::to_string(dropped) + " queued notification(s)");
                                }
                            }
                        }
                    }
                }
                connectionCallback = m_connectionCallback;
                errorCallback = m_errorCallback;
            }
            if (connectionCallback) {
                connectionCallback(deviceId, subscribed);
            }
            reports.insert(reports.begin(), {BleError::Success,
                std::string("Central ") + (subscribed ? "subscribed to " : "unsubscribed from ") +
                ToStdString(characteristic.UUID.UUIDString) + " (device " + deviceId + ")"});
            Deliver(errorCallback, reports);
        }

        // peripheralManagerIsReadyToUpdateSubscribers: CoreBluetooth has room again; send queued
        // notifications in order until it is full again.
        void HandleReadyToUpdateSubscribers() {
            ErrorCallback errorCallback;
            Reports reports;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_initialized || !m_peripheral || m_notifyQueue.Empty()) {
                    return;
                }
                const auto r = m_notifyQueue.Drain(
                    [this](const NotificationQueue::Item& it) { return SendNotificationLocked(it); });
                if (r.backlogCleared) {
                    NSLog(@"[BluetoothServer] notify queue drained (sent %zu in last batch, high water %zu)",
                        r.sent, m_notifyQueue.HighWater());
                    reports.emplace_back(BleError::Success, "Notification queue drained (sent " +
                        std::to_string(r.sent) + " in last batch, high water " +
                        std::to_string(m_notifyQueue.HighWater()) + ")");
                }
                errorCallback = m_errorCallback;
            }
            Deliver(errorCallback, reports);
        }

    private:
        void ReportError(BleError error, const std::string& message) {
            if (m_errorCallback) {
                m_errorCallback(error, message);
            }
        }

        // Call with m_mutex released.
        static void Deliver(const ErrorCallback& callback, const Reports& reports) {
            if (!callback) {
                return;
            }
            for (const auto& [error, message] : reports) {
                callback(error, message);
            }
        }

        // One updateValue attempt for a queued/new value. Requires m_mutex. Never calls user code.
        NotificationQueue::SendOutcome SendNotificationLocked(const NotificationQueue::Item& item) {
            auto* charData = GetCharacteristicData(item.service, item.characteristic);
            if (!charData || !m_peripheral) {
                return NotificationQueue::SendOutcome::Discard;
            }
            NSData* data = [NSData dataWithBytes:item.value.data() length:item.value.size()];
            if ([m_peripheral updateValue:data forCharacteristic:charData->characteristic onSubscribedCentrals:nil]) {
                return NotificationQueue::SendOutcome::Sent;
            }
            // NO = transmit queue full; a "ready" callback will follow. With no subscriber there is
            // nobody to deliver to and possibly no "ready" callback, so do not keep the value.
            return charData->subscribedDevices.empty()
                ? NotificationQueue::SendOutcome::Discard
                : NotificationQueue::SendOutcome::Busy;
        }

        bool FindServiceUuid(const CharacteristicData* charData, std::string& serviceUuid) const {
            for (const auto& [uuid, service] : m_services) {
                for (const auto& [charUuid, data] : service->characteristics) {
                    if (data.get() == charData) {
                        serviceUuid = uuid;
                        return true;
                    }
                }
            }
            return false;
        }

        void StartNativeAdvertisingLocked() {
            if (m_peripheral.isAdvertising) {
                [m_peripheral stopAdvertising];
            }

            // macOS has a very small advertisement payload. Advertising the
            // local name alone makes the peripheral easy to identify in tools
            // like nRF Connect; services are still available after connection
            // through normal GATT discovery.
            NSDictionary* advertisement = @{
                CBAdvertisementDataLocalNameKey: ToNSString(m_config.deviceName)
            };
            NSLog(@"[BluetoothServer] startAdvertising requested name='%s'", m_config.deviceName.c_str());
            ReportError(BleError::Success, "CoreBluetooth startAdvertising requested for local name " + m_config.deviceName);
            [m_peripheral startAdvertising:advertisement];
        }

    public:
        std::string DescribeStatus() const override {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::string status = "macOS BLE peripheral status:\n";
            const char* stateName = "?";
            CBManagerState state = m_peripheral ? m_peripheral.state : CBManagerStateUnknown;
            switch (state) {
                case CBManagerStateUnknown:      stateName = "Unknown"; break;
                case CBManagerStateResetting:    stateName = "Resetting"; break;
                case CBManagerStateUnsupported:  stateName = "Unsupported"; break;
                case CBManagerStateUnauthorized: stateName = "Unauthorized (grant Bluetooth permission to Unity in System Settings)"; break;
                case CBManagerStatePoweredOff:   stateName = "PoweredOff"; break;
                case CBManagerStatePoweredOn:    stateName = "PoweredOn"; break;
            }
            status += std::string("  CB state: ") + stateName + "\n";
            status += "  Initialized: " + std::string(m_initialized ? "yes" : "no") + "\n";
            status += "  Advertising (CB): " + std::string(m_peripheral && m_peripheral.isAdvertising ? "yes" : "no") + "\n";
            status += "  Pending service adds: " + std::to_string(m_pendingServiceAdds) + "\n";
            status += "  Services: " + std::to_string(m_services.size()) + "\n";
            for (const auto& [uuid, service] : m_services) {
                status += "    - " + uuid + " (" + std::to_string(service->characteristics.size()) + " chars)\n";
            }
            status += "  Queued notifications: " + std::to_string(m_notifyQueue.Size()) + " / " +
                std::to_string(m_notifyQueue.Capacity()) + " (high water " + std::to_string(m_notifyQueue.HighWater()) +
                ", dropped " + std::to_string(m_notifyQueue.TotalDropped()) + ")\n";
            status += "  Subscribed centrals: " + std::to_string(m_connectedDevices.size());
            return status;
        }

    private:

        CharacteristicData* GetCharacteristicData(const std::string& serviceUuid, const std::string& characteristicUuid) {
            auto serviceIt = m_services.find(serviceUuid);
            if (serviceIt == m_services.end()) {
                return nullptr;
            }
            auto charIt = serviceIt->second->characteristics.find(characteristicUuid);
            return charIt != serviceIt->second->characteristics.end() ? charIt->second.get() : nullptr;
        }

        CharacteristicData* FindCharacteristic(CBUUID* uuid) {
            for (const auto& [key, value] : m_characteristicsByUuid) {
                if ([key isEqual:uuid]) {
                    return value;
                }
            }
            return nullptr;
        }

        void RebuildCharacteristicIndex() {
            m_characteristicsByUuid.clear();
            for (auto& [serviceUuid, service] : m_services) {
                for (auto& [charUuid, charData] : service->characteristics) {
                    m_characteristicsByUuid[charData->characteristic.UUID] = charData.get();
                }
            }
        }

        void RebuildServiceCharacteristics(ServiceData* service) {
            NSMutableArray<CBMutableCharacteristic*>* characteristics = [NSMutableArray array];
            for (auto& [uuid, charData] : service->characteristics) {
                [characteristics addObject:charData->characteristic];
            }
            service->service.characteristics = characteristics;
        }
    };

    std::unique_ptr<IBleServer> CreatePlatformServer() {
        return std::make_unique<BleServerMacOS>();
    }

} // namespace BleServer

@implementation BlePeripheralDelegate {
    BleServer::BleServerMacOS* _owner;
}

- (instancetype)initWithOwner:(BleServer::BleServerMacOS*)owner {
    self = [super init];
    if (self) {
        _owner = owner;
    }
    return self;
}

- (void)peripheralManagerDidUpdateState:(CBPeripheralManager*)peripheral {
    (void)peripheral;
    if (_owner) {
        _owner->HandleStateUpdate();
    }
}

- (void)peripheralManager:(CBPeripheralManager*)peripheral didAddService:(CBService*)service error:(NSError*)error {
    (void)peripheral;
    if (_owner) {
        _owner->HandleServiceAdded(service, error);
    }
}

- (void)peripheralManagerDidStartAdvertising:(CBPeripheralManager*)peripheral error:(NSError*)error {
    (void)peripheral;
    if (_owner) {
        _owner->HandleAdvertisingStarted(error);
    }
}

- (void)peripheralManager:(CBPeripheralManager*)peripheral didReceiveReadRequest:(CBATTRequest*)request {
    (void)peripheral;
    if (_owner) {
        _owner->HandleRead(request);
    }
}

- (void)peripheralManager:(CBPeripheralManager*)peripheral didReceiveWriteRequests:(NSArray<CBATTRequest*>*)requests {
    (void)peripheral;
    if (_owner) {
        _owner->HandleWrite(requests);
    }
}

- (void)peripheralManager:(CBPeripheralManager*)peripheral central:(CBCentral*)central didSubscribeToCharacteristic:(CBCharacteristic*)characteristic {
    (void)peripheral;
    if (_owner) {
        _owner->HandleSubscribe(central, characteristic, true);
    }
}

- (void)peripheralManager:(CBPeripheralManager*)peripheral central:(CBCentral*)central didUnsubscribeFromCharacteristic:(CBCharacteristic*)characteristic {
    (void)peripheral;
    if (_owner) {
        _owner->HandleSubscribe(central, characteristic, false);
    }
}

- (void)peripheralManagerIsReadyToUpdateSubscribers:(CBPeripheralManager*)peripheral {
    (void)peripheral;
    if (_owner) {
        _owner->HandleReadyToUpdateSubscribers();
    }
}

@end
