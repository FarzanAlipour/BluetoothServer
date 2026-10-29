// BleServerWindows.cpp - Clean BLE Server implementation for Windows
#include "IBleServer.h"
#include "BleUtilities.h"
#include "OrderedDelivery.h"
#include "PeerSendGate.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <thread>
#include <condition_variable>
#include <sstream>
#include <iomanip>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;

namespace BleServer {

    // Helper function to convert HRESULT to readable error message
    static std::string HResultToString(hresult hr) {
        std::stringstream ss;
        ss << "HRESULT 0x" << std::hex << std::setw(8) << std::setfill('0') << static_cast<uint32_t>(hr);

        // Common HRESULT error codes
        switch (static_cast<uint32_t>(hr)) {
            case 0x8000000E: return ss.str() + " (E_POINTER: Invalid pointer)";
            case 0x80004001: return ss.str() + " (E_NOTIMPL: Not implemented)";
            case 0x80004002: return ss.str() + " (E_NOINTERFACE: No such interface)";
            case 0x80004003: return ss.str() + " (E_POINTER: Invalid pointer)";
            case 0x80004004: return ss.str() + " (E_ABORT: Operation aborted)";
            case 0x80004005: return ss.str() + " (E_FAIL: Unspecified failure)";
            case 0x8007000E: return ss.str() + " (E_OUTOFMEMORY: Out of memory)";
            case 0x80070057: return ss.str() + " (E_INVALIDARG: Invalid argument)";
            case 0x8000FFFF: return ss.str() + " (E_UNEXPECTED: Unexpected failure)";
            case 0x800710DD: return ss.str() + " (ERROR_DEVICE_NOT_AVAILABLE: Device not available)";
            case 0x80070490: return ss.str() + " (ERROR_NOT_FOUND: Element not found)";
            default: return ss.str();
        }
    }

    class BleServerWindows : public IBleServer {
    private:
        struct CharacteristicData {
            GattLocalCharacteristic characteristic = nullptr;
            CharacteristicConfig config;
            CharacteristicReadCallback readCallback;
            CharacteristicWriteCallback writeCallback;
            std::vector<uint8_t> value;
            std::mutex valueMutex;
            event_token readToken{};
            event_token writeToken{};
            event_token subscriptionToken{};
            // Centrals currently subscribed to this characteristic (deviceId -> WinRT client).
            std::map<std::string, GattSubscribedClient> subscribers;
        };

        struct ServiceData {
            GattServiceProvider provider = nullptr;
            ServiceConfig config;
            std::map<std::string, std::unique_ptr<CharacteristicData>> characteristics;
        };

        // docs/PROTOCOL.md 6.2: a Windows server awaits the previous NotifyValueAsync before the next.
        static constexpr size_t kMaxNotifyInFlightPerCentral = 1;

        // A central with at least one subscription: its GATT session (for MaxPduSize) and the
        // MaxPduSizeChanged registration.
        struct PeerSession {
            GattSession session = nullptr;
            event_token mtuToken{};
            size_t maxPayload = 0;  // MaxPduSize - 3, 0 = not known
        };

        // NotifyPeer flow-control state. Shared (shared_ptr) with NotifyValueAsync completion
        // handlers, which may run on a thread pool thread after this object is gone.
        struct SendState {
            std::mutex mutex;
            PeerSendGate gate{ kMaxNotifyInFlightPerCentral };
            ReadyToSendCallback readyCallback;
            ErrorCallback errorCallback;
            bool alive = true;
        };

        struct WriteDelivery {
            CharacteristicWriteCallback callback;
            std::string deviceId;
            std::vector<uint8_t> data;
        };

        // Core members
        ServerConfig m_config;
        bool m_initialized = false;
        bool m_advertising = false;
        std::map<std::string, std::unique_ptr<ServiceData>> m_services;
        std::map<hstring, std::string> m_sessionToDeviceId;
        std::map<std::string, hstring> m_deviceToSessionId;
        std::map<std::string, PeerSession> m_peers;  // deviceId -> session of a subscribed central
        std::shared_ptr<SendState> m_send = std::make_shared<SendState>();
        // WriteRequested events are raised in order, but each body arrives through an async
        // GetRequestAsync(): this puts the write callbacks back in event order, one at a time.
        OrderedDelivery<WriteDelivery> m_writeOrder;

        // Callbacks
        ConnectionCallback m_connectionCallback;
        ErrorCallback m_errorCallback;
        SubscriptionCallback m_subscriptionCallback;
        MaxPayloadCallback m_maxPayloadCallback;

        // Thread safety
        mutable std::mutex m_mutex;

        // Windows specific
        BluetoothAdapter m_adapter = nullptr;
        std::thread m_gattThread;
        std::atomic<bool> m_running{ false };
        std::condition_variable m_initCondition;

    public:
        BleServerWindows() {
            // Initialize WinRT on this thread
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        }

        ~BleServerWindows() {
            Shutdown();
        }

        BleError Initialize(const ServerConfig& config) override {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (m_initialized) {
                return BleError::AlreadyInitialized;
            }

            // Validate configuration
            if (!Utils::IsValidDeviceName(config.deviceName)) {
                ReportError(BleError::InvalidParameter, "Invalid device name: must be 1-248 characters");
                return BleError::InvalidParameter;
            }

            if (!Utils::IsValidMaxConnections(config.maxConnections)) {
                ReportError(BleError::InvalidParameter, "Invalid max connections: must be 1-10");
                return BleError::InvalidParameter;
            }

            m_config = config;

            try {
                // Get Bluetooth adapter
                auto adapterOp = BluetoothAdapter::GetDefaultAsync();
                m_adapter = adapterOp.get();

                if (!m_adapter) {
                    ReportError(BleError::NotSupported, "Bluetooth adapter not found");
                    return BleError::NotSupported;
                }

                if (!m_adapter.IsLowEnergySupported()) {
                    ReportError(BleError::NotSupported, "Bluetooth LE not supported");
                    return BleError::NotSupported;
                }

                // Check if Bluetooth is enabled
                if (!IsBluetoothEnabled()) {
                    ReportError(BleError::BluetoothDisabled, "Bluetooth is disabled");
                    return BleError::BluetoothDisabled;
                }

                {
                    std::lock_guard<std::mutex> sendLock(m_send->mutex);
                    m_send->alive = true;
                }
                m_initialized = true;
                return BleError::Success;
            }
            catch (const hresult_error& ex) {
                std::string errorMsg = "Failed to initialize: " + HResultToString(ex.code()) + " - " + winrt::to_string(ex.message());
                ReportError(BleError::Unknown, errorMsg);
                return BleError::Unknown;
            }
        }

        void Shutdown() override {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (!m_initialized) {
                return;
            }

            // Stop advertising (m_mutex is already held: StopAdvertising() would lock it again)
            StopAdvertisingLocked();

            // Per-central transport: no ReadyToSend after Shutdown, no pending write deliveries.
            {
                std::lock_guard<std::mutex> sendLock(m_send->mutex);
                m_send->alive = false;
                m_send->gate.Clear();
            }
            m_writeOrder.Reset();
            for (auto& [deviceId, peer] : m_peers) {
                RevokeMtuHandler(peer);
            }
            m_peers.clear();

            // Clean up all services
            for (auto& [uuid, service] : m_services) {
                if (service->provider) {
                    try {
                        if (service->provider.AdvertisementStatus() ==
                            GattServiceProviderAdvertisementStatus::Started) {
                            service->provider.StopAdvertising();
                        }
                    }
                    catch (...) {}
                }
            }
            m_services.clear();

            // Clear device mappings
            m_sessionToDeviceId.clear();
            m_deviceToSessionId.clear();

            m_initialized = false;
            m_adapter = nullptr;
        }

        bool IsInitialized() const override {
            return m_initialized;
        }

        BleError AddService(const ServiceConfig& config) override {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (!m_initialized) {
                return BleError::NotInitialized;
            }

            // Validate UUID
            if (!Utils::IsValidUuid(config.uuid)) {
                ReportError(BleError::InvalidParameter, "Invalid service UUID format: " + config.uuid);
                return BleError::InvalidParameter;
            }

            if (m_services.find(config.uuid) != m_services.end()) {
                ReportError(BleError::InvalidParameter, "Service already exists: " + config.uuid);
                return BleError::InvalidParameter;
            }

            try {
                guid serviceGuid(config.uuid);
                auto providerResult = GattServiceProvider::CreateAsync(serviceGuid).get();

                if (providerResult.Error() != BluetoothError::Success) {
                    ReportError(BleError::Unknown, "Failed to create service provider for UUID: " + config.uuid);
                    return BleError::Unknown;
                }

                auto serviceData = std::make_unique<ServiceData>();
                serviceData->provider = providerResult.ServiceProvider();
                serviceData->config = config;

                m_services[config.uuid] = std::move(serviceData);
                return BleError::Success;
            }
            catch (const hresult_error& ex) {
                std::string errorMsg = "Failed to add service " + config.uuid + ": " + HResultToString(ex.code()) + " - " + winrt::to_string(ex.message());
                ReportError(BleError::Unknown, errorMsg);
                return BleError::Unknown;
            }
        }

        BleError RemoveService(const std::string& serviceUuid) override {
            std::lock_guard<std::mutex> lock(m_mutex);

            auto it = m_services.find(serviceUuid);
            if (it == m_services.end()) {
                return BleError::ServiceNotFound;
            }

            if (it->second->provider) {
                try {
                    it->second->provider.StopAdvertising();
                }
                catch (...) {}
            }

            std::set<std::string> subscribers;
            for (const auto& [charUuid, charData] : it->second->characteristics) {
                for (const auto& [deviceId, client] : charData->subscribers) {
                    subscribers.insert(deviceId);
                }
            }
            m_services.erase(it);
            PrunePeersLocked(subscribers);
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

        BleError AddCharacteristic(const std::string& serviceUuid,
            const CharacteristicConfig& config) override {
            std::lock_guard<std::mutex> lock(m_mutex);

            auto serviceIt = m_services.find(serviceUuid);
            if (serviceIt == m_services.end()) {
                return BleError::ServiceNotFound;
            }

            // Validate UUID
            if (!Utils::IsValidUuid(config.uuid)) {
                ReportError(BleError::InvalidParameter, "Invalid characteristic UUID format: " + config.uuid);
                return BleError::InvalidParameter;
            }

            // Validate properties
            if (!Utils::IsValidProperties(config.properties)) {
                ReportError(BleError::InvalidParameter, "Invalid characteristic properties flags");
                return BleError::InvalidParameter;
            }

            // Validate permissions
            if (!Utils::IsValidPermissions(config.permissions)) {
                ReportError(BleError::InvalidParameter, "Invalid characteristic permissions flags");
                return BleError::InvalidParameter;
            }

            // Validate initial value size
            if (!config.initialValue.empty() && !Utils::IsValidValueSize(config.initialValue.size(), config.maxLength)) {
                ReportError(BleError::InvalidParameter, "Initial value size exceeds maxLength");
                return BleError::InvalidParameter;
            }

            auto& service = serviceIt->second;
            if (service->characteristics.find(config.uuid) != service->characteristics.end()) {
                ReportError(BleError::InvalidParameter, "Characteristic already exists: " + config.uuid);
                return BleError::InvalidParameter;
            }

            try {
                GattLocalCharacteristicParameters params;

                // Convert properties
                GattCharacteristicProperties props = GattCharacteristicProperties::None;
                if (config.properties & static_cast<uint32_t>(CharacteristicProperty::Read)) {
                    props |= GattCharacteristicProperties::Read;
                }
                if (config.properties & static_cast<uint32_t>(CharacteristicProperty::Write)) {
                    props |= GattCharacteristicProperties::Write;
                }
                if (config.properties & static_cast<uint32_t>(CharacteristicProperty::Notify)) {
                    props |= GattCharacteristicProperties::Notify;
                }
                if (config.properties & static_cast<uint32_t>(CharacteristicProperty::Indicate)) {
                    props |= GattCharacteristicProperties::Indicate;
                }
                if (config.properties & static_cast<uint32_t>(CharacteristicProperty::WriteWithoutResponse)) {
                    props |= GattCharacteristicProperties::WriteWithoutResponse;
                }

                params.CharacteristicProperties(props);

                // Set protection level based on permissions
                if (config.permissions & static_cast<uint32_t>(CharacteristicPermission::ReadEncrypted) ||
                    config.permissions & static_cast<uint32_t>(CharacteristicPermission::WriteEncrypted)) {
                    params.ReadProtectionLevel(GattProtectionLevel::EncryptionRequired);
                    params.WriteProtectionLevel(GattProtectionLevel::EncryptionRequired);
                }
                else {
                    params.ReadProtectionLevel(GattProtectionLevel::Plain);
                    params.WriteProtectionLevel(GattProtectionLevel::Plain);
                }

                guid charGuid(config.uuid);
                auto charResult = service->provider.Service().CreateCharacteristicAsync(
                    charGuid, params).get();

                if (charResult.Error() != BluetoothError::Success) {
                    ReportError(BleError::Unknown, "Failed to create characteristic: " + config.uuid);
                    return BleError::Unknown;
                }

                auto charData = std::make_unique<CharacteristicData>();
                charData->characteristic = charResult.Characteristic();
                charData->config = config;
                charData->value = config.initialValue;

                // Set up handlers
                SetupCharacteristicHandlers(serviceUuid, config.uuid, charData.get());

                service->characteristics[config.uuid] = std::move(charData);
                return BleError::Success;
            }
            catch (const hresult_error& ex) {
                std::string errorMsg = "Failed to add characteristic " + config.uuid + ": " + HResultToString(ex.code()) + " - " + winrt::to_string(ex.message());
                ReportError(BleError::Unknown, errorMsg);
                return BleError::Unknown;
            }
        }

        BleError RemoveCharacteristic(const std::string& serviceUuid,
            const std::string& characteristicUuid) override {
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

            auto& charData = charIt->second;

            try {
                // Unsubscribe event handlers
                if (charData->readToken.value != 0) {
                    charData->characteristic.ReadRequested(charData->readToken);
                    charData->readToken = {};
                }

                if (charData->writeToken.value != 0) {
                    charData->characteristic.WriteRequested(charData->writeToken);
                    charData->writeToken = {};
                }

                if (charData->subscriptionToken.value != 0) {
                    charData->characteristic.SubscribedClientsChanged(charData->subscriptionToken);
                    charData->subscriptionToken = {};
                }

                // Note: Windows Runtime GATT doesn't provide a direct way to remove a characteristic
                // from a service once it's created. The characteristic will be cleaned up when
                // the service is removed or when the server shuts down.
                // We remove it from our tracking to prevent further use.

                std::set<std::string> subscribers;
                for (const auto& [deviceId, client] : charData->subscribers) {
                    subscribers.insert(deviceId);
                }
                service->characteristics.erase(charIt);
                PrunePeersLocked(subscribers);
                return BleError::Success;
            }
            catch (const hresult_error& ex) {
                std::string errorMsg = "Failed to remove characteristic " + characteristicUuid + ": " + HResultToString(ex.code()) + " - " + winrt::to_string(ex.message());
                ReportError(BleError::Unknown, errorMsg);
                // Still remove from tracking even if cleanup failed
                service->characteristics.erase(charIt);
                return BleError::Success;
            }
        }

        BleError SetCharacteristicReadCallback(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            CharacteristicReadCallback callback) override {
            std::lock_guard<std::mutex> lock(m_mutex);

            auto charData = GetCharacteristicData(serviceUuid, characteristicUuid);
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

            auto charData = GetCharacteristicData(serviceUuid, characteristicUuid);
            if (!charData) {
                return BleError::CharacteristicNotFound;
            }

            charData->writeCallback = callback;
            return BleError::Success;
        }

        BleError StartAdvertising() override {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (!m_initialized) {
                return BleError::NotInitialized;
            }

            if (m_advertising) {
                return BleError::Success;
            }

            try {
                for (auto& [uuid, service] : m_services) {
                    if (service->provider) {
                        GattServiceProviderAdvertisingParameters advParams;
                        advParams.IsDiscoverable(true);
                        advParams.IsConnectable(true);

                        service->provider.StartAdvertising(advParams);
                    }
                }

                m_advertising = true;
                return BleError::Success;
            }
            catch (const hresult_error& ex) {
                ReportError(BleError::AdvertisingFailed, winrt::to_string(ex.message()));
                return BleError::AdvertisingFailed;
            }
        }

        BleError StopAdvertising() override {
            std::lock_guard<std::mutex> lock(m_mutex);
            return StopAdvertisingLocked();
        }

        // Requires m_mutex.
        BleError StopAdvertisingLocked() {
            if (!m_advertising) {
                return BleError::Success;
            }

            for (auto& [uuid, service] : m_services) {
                if (service->provider &&
                    service->provider.AdvertisementStatus() == GattServiceProviderAdvertisementStatus::Started) {
                    try {
                        service->provider.StopAdvertising();
                    }
                    catch (...) {}
                }
            }

            m_advertising = false;
            return BleError::Success;
        }

        bool IsAdvertising() const override {
            return m_advertising;
        }

        void SetConnectionCallback(ConnectionCallback callback) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_connectionCallback = callback;
        }

        std::vector<std::string> GetConnectedDevices() const override {
            std::lock_guard<std::mutex> lock(m_mutex);

            std::vector<std::string> devices;
            for (const auto& [deviceId, sessionId] : m_deviceToSessionId) {
                devices.push_back(deviceId);
            }
            return devices;
        }

        BleError DisconnectDevice(const std::string& deviceId) override {
            // Note: In BLE server mode, we can't forcefully disconnect clients
            // We can only stop advertising and wait for them to disconnect
            return BleError::NotSupported;
        }

        BleError UpdateCharacteristicValue(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            const std::vector<uint8_t>& value) override {
            std::lock_guard<std::mutex> lock(m_mutex);

            auto charData = GetCharacteristicData(serviceUuid, characteristicUuid);
            if (!charData) {
                return BleError::CharacteristicNotFound;
            }

            // Validate value size
            if (!value.empty() && !Utils::IsValidValueSize(value.size(), charData->config.maxLength)) {
                ReportError(BleError::InvalidParameter, "Value size " + std::to_string(value.size()) +
                    " exceeds maxLength " + std::to_string(charData->config.maxLength));
                return BleError::InvalidParameter;
            }

            {
                std::lock_guard<std::mutex> valueLock(charData->valueMutex);
                charData->value = value;
            }

            return BleError::Success;
        }

        BleError NotifyCharacteristicChanged(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            const std::string& deviceId) override {
            std::lock_guard<std::mutex> lock(m_mutex);

            auto charData = GetCharacteristicData(serviceUuid, characteristicUuid);
            if (!charData) {
                return BleError::CharacteristicNotFound;
            }

            try {
                auto clients = charData->characteristic.SubscribedClients();
                if (clients.Size() == 0) {
                    return BleError::Success; // No one to notify
                }

                DataWriter writer;
                {
                    std::lock_guard<std::mutex> valueLock(charData->valueMutex);
                    writer.WriteBytes(charData->value);
                }
                IBuffer buffer = writer.DetachBuffer();

                if (deviceId.empty()) {
                    // Notify all subscribed clients
                    for (const auto& client : clients) {
                        charData->characteristic.NotifyValueAsync(buffer, client);
                    }
                }
                else {
                    // Notify specific client
                    auto it = m_deviceToSessionId.find(deviceId);
                    if (it != m_deviceToSessionId.end()) {
                        for (const auto& client : clients) {
                            if (client.Session().DeviceId().Id() == it->second) {
                                charData->characteristic.NotifyValueAsync(buffer, client);
                                break;
                            }
                        }
                    }
                }

                return BleError::Success;
            }
            catch (const hresult_error& ex) {
                ReportError(BleError::Unknown, winrt::to_string(ex.message()));
                return BleError::Unknown;
            }
        }

        void SetErrorCallback(ErrorCallback callback) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_errorCallback = callback;
            std::lock_guard<std::mutex> sendLock(m_send->mutex);
            m_send->errorCallback = std::move(callback);
        }

        // One notification to one central: NotifyValueAsync(value, GattSubscribedClient). At most
        // kMaxNotifyInFlightPerCentral calls are outstanding per central; a further call returns
        // Busy and the completion of the outstanding one fires ReadyToSend(deviceId).
        NotifyStatus NotifyPeer(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            const std::string& deviceId,
            const uint8_t* data,
            size_t length) override {
            GattLocalCharacteristic characteristic = nullptr;
            GattSubscribedClient client = nullptr;
            std::shared_ptr<SendState> send;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_initialized || deviceId.empty() || (!data && length > 0)) {
                    return NotifyStatus::Error;
                }
                auto* charData = GetCharacteristicData(serviceUuid, characteristicUuid);
                if (!charData || !charData->characteristic) {
                    return NotifyStatus::Error;
                }
                auto subIt = charData->subscribers.find(deviceId);
                if (subIt == charData->subscribers.end()) {
                    return NotifyStatus::NotSubscribed;
                }
                auto peerIt = m_peers.find(deviceId);
                const size_t limit = peerIt == m_peers.end() ? 0 : peerIt->second.maxPayload;
                if (limit > 0 && length > limit) {
                    return NotifyStatus::TooLarge;
                }
                characteristic = charData->characteristic;
                client = subIt->second;
                send = m_send;
            }

            {
                std::lock_guard<std::mutex> sendLock(send->mutex);
                if (!send->alive) {
                    return NotifyStatus::Error;
                }
                if (!send->gate.TryAcquire(deviceId)) {
                    return NotifyStatus::Busy;  // gate remembered deviceId: ReadyToSend follows
                }
            }

            try {
                DataWriter writer;
                if (length > 0) {
                    writer.WriteBytes(array_view<uint8_t const>(data, data + length));
                }
                auto op = characteristic.NotifyValueAsync(writer.DetachBuffer(), client);
                op.Completed([send, deviceId](IAsyncOperation<GattClientNotificationResult> const& completed,
                    AsyncStatus status) {
                        std::string failure;
                        try {
                            if (status != AsyncStatus::Completed) {
                                failure = "NotifyValueAsync did not complete (status " +
                                    std::to_string(static_cast<int>(status)) + ")";
                            }
                            else if (completed.GetResults().Status() != GattCommunicationStatus::Success) {
                                failure = "NotifyValueAsync failed (GattCommunicationStatus " +
                                    std::to_string(static_cast<int>(completed.GetResults().Status())) + ")";
                            }
                        }
                        catch (const hresult_error& ex) {
                            failure = "NotifyValueAsync failed: " + HResultToString(ex.code());
                        }
                        ReadyToSendCallback ready;
                        ErrorCallback error;
                        {
                            std::lock_guard<std::mutex> sendLock(send->mutex);
                            const bool owed = send->gate.Release(deviceId);
                            if (!send->alive) {
                                return;
                            }
                            if (owed) {
                                ready = send->readyCallback;
                            }
                            if (!failure.empty()) {
                                error = send->errorCallback;
                            }
                        }
                        if (error) {
                            error(BleError::Unknown, failure + " for device " + deviceId);
                        }
                        if (ready) {
                            ready(deviceId);
                        }
                    });
                return NotifyStatus::Sent;
            }
            catch (const hresult_error& ex) {
                ErrorCallback error;
                {
                    std::lock_guard<std::mutex> sendLock(send->mutex);
                    send->gate.Cancel(deviceId);
                    error = send->errorCallback;
                }
                if (error) {
                    error(BleError::Unknown, "NotifyValueAsync: " + HResultToString(ex.code()) + " - " +
                        winrt::to_string(ex.message()));
                }
                return NotifyStatus::Error;
            }
        }

        size_t GetMaxNotifyPayload(const std::string& deviceId) const override {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_peers.find(deviceId);
            return it == m_peers.end() ? 0 : it->second.maxPayload;
        }

        std::vector<std::string> GetSubscribers(const std::string& serviceUuid,
            const std::string& characteristicUuid) const override {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::vector<std::string> out;
            auto serviceIt = m_services.find(serviceUuid);
            if (serviceIt == m_services.end()) {
                return out;
            }
            auto charIt = serviceIt->second->characteristics.find(characteristicUuid);
            if (charIt == serviceIt->second->characteristics.end()) {
                return out;
            }
            for (const auto& [deviceId, client] : charIt->second->subscribers) {
                out.push_back(deviceId);
            }
            return out;
        }

        void SetSubscriptionCallback(SubscriptionCallback callback) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_subscriptionCallback = std::move(callback);
        }

        void SetReadyToSendCallback(ReadyToSendCallback callback) override {
            std::lock_guard<std::mutex> sendLock(m_send->mutex);
            m_send->readyCallback = std::move(callback);
        }

        void SetMaxPayloadCallback(MaxPayloadCallback callback) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_maxPayloadCallback = std::move(callback);
        }

        uint32_t GetCapabilities() const override {
            return static_cast<uint32_t>(ServerCapability::Advertising) |
                static_cast<uint32_t>(ServerCapability::Connections) |
                static_cast<uint32_t>(ServerCapability::Read) |
                static_cast<uint32_t>(ServerCapability::Write) |
                static_cast<uint32_t>(ServerCapability::Notify) |
                static_cast<uint32_t>(ServerCapability::Indicate) |
                static_cast<uint32_t>(ServerCapability::Encryption) |
                static_cast<uint32_t>(ServerCapability::Chunking) |
                static_cast<uint32_t>(ServerCapability::PeerNotify);
        }

    private:
        CharacteristicData* GetCharacteristicData(const std::string& serviceUuid,
            const std::string& characteristicUuid) {
            auto serviceIt = m_services.find(serviceUuid);
            if (serviceIt == m_services.end()) {
                return nullptr;
            }

            auto charIt = serviceIt->second->characteristics.find(characteristicUuid);
            if (charIt == serviceIt->second->characteristics.end()) {
                return nullptr;
            }

            return charIt->second.get();
        }

        void SetupCharacteristicHandlers(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            CharacteristicData* charData) {
            // Subscription changed handler
            charData->subscriptionToken = charData->characteristic.SubscribedClientsChanged(
                [this, serviceUuid, characteristicUuid](GattLocalCharacteristic const& sender,
                    IInspectable const&) {
                        HandleSubscriptionChanged(serviceUuid, characteristicUuid, sender);
                });

            // Read request handler
            if (charData->config.properties & static_cast<uint32_t>(CharacteristicProperty::Read)) {
                charData->readToken = charData->characteristic.ReadRequested(
                    [this, serviceUuid, characteristicUuid](GattLocalCharacteristic const& sender,
                        GattReadRequestedEventArgs const& args) {
                            HandleReadRequest(serviceUuid, characteristicUuid, args);
                    });
            }

            // Write request handler
            if (charData->config.properties & (static_cast<uint32_t>(CharacteristicProperty::Write) |
                static_cast<uint32_t>(CharacteristicProperty::WriteWithoutResponse))) {
                charData->writeToken = charData->characteristic.WriteRequested(
                    [this, serviceUuid, characteristicUuid](GattLocalCharacteristic const& sender,
                        GattWriteRequestedEventArgs const& args) {
                            HandleWriteRequest(serviceUuid, characteristicUuid, args);
                    });
            }
        }

        void HandleSubscriptionChanged(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            GattLocalCharacteristic const& characteristic) {
            // WinRT only reports "the subscriber list of this characteristic changed": diff it
            // against the previous list to get per-central subscribe/unsubscribe events.
            // A central counts as connected while it is subscribed to at least one characteristic
            // (WinRT has no peripheral-side connection event).
            std::map<std::string, GattSubscribedClient> current;
            try {
                for (const auto& client : characteristic.SubscribedClients()) {
                    current.emplace(DeviceIdFor(client.Session()), client);
                }
            }
            catch (const hresult_error& ex) {
                std::lock_guard<std::mutex> lock(m_mutex);
                ReportError(BleError::Unknown, "SubscribedClients: " + HResultToString(ex.code()));
                return;
            }

            std::vector<std::string> added;
            std::vector<std::string> removed;
            std::vector<std::string> connected;
            std::vector<std::string> disconnected;
            std::vector<std::pair<std::string, size_t>> maxPayloads;
            ConnectionCallback connectionCallback;
            SubscriptionCallback subscriptionCallback;
            MaxPayloadCallback maxPayloadCallback;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto* charData = GetCharacteristicData(serviceUuid, characteristicUuid);
                if (!charData || !m_initialized) {
                    return;
                }
                for (const auto& [deviceId, client] : current) {
                    if (charData->subscribers.count(deviceId) == 0) {
                        added.push_back(deviceId);
                    }
                }
                for (const auto& [deviceId, client] : charData->subscribers) {
                    if (current.count(deviceId) == 0) {
                        removed.push_back(deviceId);
                    }
                }
                charData->subscribers = current;

                for (const auto& deviceId : added) {
                    if (m_peers.count(deviceId) != 0) {
                        continue;
                    }
                    const GattSession session = current.at(deviceId).Session();
                    PeerSession& peer = m_peers[deviceId];
                    peer.session = session;
                    peer.maxPayload = MaxPayloadOf(session);
                    try {
                        peer.mtuToken = session.MaxPduSizeChanged(
                            [this, deviceId](GattSession const& sender, IInspectable const&) {
                                HandleMaxPduSizeChanged(deviceId, sender);
                            });
                    }
                    catch (...) {
                        peer.mtuToken = {};
                    }
                    const hstring sessionId = session.DeviceId().Id();
                    m_sessionToDeviceId[sessionId] = deviceId;
                    m_deviceToSessionId[deviceId] = sessionId;
                    connected.push_back(deviceId);
                    if (peer.maxPayload > 0) {
                        maxPayloads.emplace_back(deviceId, peer.maxPayload);
                    }
                }
                for (const auto& deviceId : removed) {
                    if (!HasAnySubscriptionLocked(deviceId) && m_peers.count(deviceId) != 0) {
                        ForgetPeerLocked(deviceId);
                        disconnected.push_back(deviceId);
                    }
                }
                connectionCallback = m_connectionCallback;
                subscriptionCallback = m_subscriptionCallback;
                maxPayloadCallback = m_maxPayloadCallback;
            }

            // Same order as macOS: connection, max payload, then the subscription itself.
            for (const auto& deviceId : connected) {
                if (connectionCallback) {
                    connectionCallback(deviceId, true);
                }
            }
            for (const auto& [deviceId, size] : maxPayloads) {
                if (maxPayloadCallback) {
                    maxPayloadCallback(deviceId, size);
                }
            }
            for (const auto& deviceId : added) {
                if (subscriptionCallback) {
                    subscriptionCallback(serviceUuid, characteristicUuid, deviceId, true);
                }
            }
            for (const auto& deviceId : removed) {
                if (subscriptionCallback) {
                    subscriptionCallback(serviceUuid, characteristicUuid, deviceId, false);
                }
            }
            for (const auto& deviceId : disconnected) {
                if (connectionCallback) {
                    connectionCallback(deviceId, false);
                }
            }
        }

        void HandleMaxPduSizeChanged(const std::string& deviceId, GattSession const& session) {
            MaxPayloadCallback callback;
            size_t value = 0;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_peers.find(deviceId);
                if (it == m_peers.end()) {
                    return;
                }
                value = MaxPayloadOf(session);
                if (value == 0 || value == it->second.maxPayload) {
                    return;
                }
                it->second.maxPayload = value;
                callback = m_maxPayloadCallback;
            }
            if (callback) {
                callback(deviceId, value);
            }
        }

        // ATT_MTU - 3 from GattSession.MaxPduSize, 0 when unknown.
        static size_t MaxPayloadOf(GattSession const& session) {
            try {
                const size_t pdu = session ? static_cast<size_t>(session.MaxPduSize()) : 0;
                return pdu > 3 ? pdu - 3 : 0;
            }
            catch (...) {
                return 0;
            }
        }

        // Stable per-process id of a central (same scheme as before: hash of the WinRT device id).
        static std::string DeviceIdFor(GattSession const& session) {
            return Utils::GenerateDeviceId(winrt::to_string(session.DeviceId().Id()));
        }

        bool HasAnySubscriptionLocked(const std::string& deviceId) const {
            for (const auto& [serviceUuid, service] : m_services) {
                for (const auto& [charUuid, charData] : service->characteristics) {
                    if (charData->subscribers.count(deviceId) != 0) {
                        return true;
                    }
                }
            }
            return false;
        }

        static void RevokeMtuHandler(PeerSession& peer) {
            if (peer.session && peer.mtuToken.value != 0) {
                try {
                    peer.session.MaxPduSizeChanged(peer.mtuToken);
                }
                catch (...) {}
            }
            peer.mtuToken = {};
        }

        // The central has no subscription left: drop its session and any owed ReadyToSend.
        void ForgetPeerLocked(const std::string& deviceId) {
            auto it = m_peers.find(deviceId);
            if (it != m_peers.end()) {
                RevokeMtuHandler(it->second);
                m_peers.erase(it);
            }
            auto sessionIt = m_deviceToSessionId.find(deviceId);
            if (sessionIt != m_deviceToSessionId.end()) {
                m_sessionToDeviceId.erase(sessionIt->second);
                m_deviceToSessionId.erase(sessionIt);
            }
            std::lock_guard<std::mutex> sendLock(m_send->mutex);
            m_send->gate.Forget(deviceId);
        }

        // After characteristics went away: forget centrals with no subscription left.
        void PrunePeersLocked(const std::set<std::string>& devices) {
            for (const auto& deviceId : devices) {
                if (!HasAnySubscriptionLocked(deviceId)) {
                    ForgetPeerLocked(deviceId);
                }
            }
        }

        fire_and_forget HandleReadRequest(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            GattReadRequestedEventArgs args) {
            auto deferral = args.GetDeferral();

            // Capture all necessary data while holding the mutex before co_await
            CharacteristicReadCallback readCallback;
            std::vector<uint8_t> cachedValue;
            std::string deviceId;
            bool hasCallback = false;
            bool foundChar = false;

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto charData = GetCharacteristicData(serviceUuid, characteristicUuid);
                if (charData) {
                    foundChar = true;
                    readCallback = charData->readCallback;
                    hasCallback = (readCallback != nullptr);

                    // Also known for a central that has not subscribed to anything (yet).
                    try {
                        deviceId = DeviceIdFor(args.Session());
                    }
                    catch (...) {}

                    if (!hasCallback) {
                        std::lock_guard<std::mutex> valueLock(charData->valueMutex);
                        cachedValue = charData->value;
                    }
                }
            }

            auto request = co_await args.GetRequestAsync();

            try {
                if (!foundChar) {
                    request.RespondWithProtocolError(GattProtocolError::InvalidHandle());
                    deferral.Complete();
                    co_return;
                }

                std::vector<uint8_t> value;

                if (hasCallback && !deviceId.empty()) {
                    value = readCallback(deviceId);
                }
                else {
                    value = cachedValue;
                }

                DataWriter writer;
                writer.WriteBytes(value);
                request.RespondWithValue(writer.DetachBuffer());
            }
            catch (...) {
                request.RespondWithProtocolError(GattProtocolError::UnlikelyError());
            }

            deferral.Complete();
        }

        fire_and_forget HandleWriteRequest(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            GattWriteRequestedEventArgs args) {
            auto deferral = args.GetDeferral();

            // Runs synchronously inside the WriteRequested event (before the first co_await), so
            // tickets follow the order in which the OS raised the writes.
            const uint64_t ticket = m_writeOrder.Reserve();
            std::string deviceId;
            try {
                deviceId = DeviceIdFor(args.Session());
            }
            catch (...) {}

            GattWriteRequest request = nullptr;
            try {
                request = co_await args.GetRequestAsync();
            }
            catch (...) {
                request = nullptr;
            }

            std::optional<WriteDelivery> delivery;
            bool found = false;
            try {
                if (request) {
                    auto reader = DataReader::FromBuffer(request.Value());
                    std::vector<uint8_t> data(reader.UnconsumedBufferLength());
                    reader.ReadBytes(data);

                    std::lock_guard<std::mutex> lock(m_mutex);
                    auto* charData = GetCharacteristicData(serviceUuid, characteristicUuid);
                    if (charData) {
                        found = true;
                        {
                            std::lock_guard<std::mutex> valueLock(charData->valueMutex);
                            charData->value = data;
                        }
                        if (charData->writeCallback && !deviceId.empty()) {
                            delivery = WriteDelivery{ charData->writeCallback, deviceId, std::move(data) };
                        }
                    }
                }
            }
            catch (...) {
                delivery.reset();
            }

            // Every reserved ticket is completed (an empty one when there is nothing to deliver),
            // otherwise later writes would wait forever. The callback runs without m_mutex.
            m_writeOrder.Complete(ticket, std::move(delivery), [](WriteDelivery& w) {
                try {
                    w.callback(w.deviceId, w.data);
                }
                catch (...) {}
            });

            if (request) {
                try {
                    if (request.Option() == GattWriteOption::WriteWithResponse) {
                        if (found) {
                            request.Respond();
                        }
                        else {
                            request.RespondWithProtocolError(GattProtocolError::InvalidHandle());
                        }
                    }
                }
                catch (...) {}
            }

            deferral.Complete();
        }

        bool IsBluetoothEnabled() {
            try {
                auto radiosOp = Windows::Devices::Radios::Radio::GetRadiosAsync();
                auto radios = radiosOp.get();

                for (const auto& radio : radios) {
                    if (radio.Kind() == Windows::Devices::Radios::RadioKind::Bluetooth) {
                        return radio.State() == Windows::Devices::Radios::RadioState::On;
                    }
                }
            }
            catch (...) {
                // Fallback to adapter check
            }

            return m_adapter != nullptr;
        }

        void ReportError(BleError error, const std::string& message) {
            if (m_errorCallback) {
                m_errorCallback(error, message);
            }
        }
    };

    // Platform-specific factory function
    std::unique_ptr<IBleServer> CreatePlatformServer() {
        return std::make_unique<BleServerWindows>();
    }

} // namespace BleServer