// BleServerWindows.cpp - Clean BLE Server implementation for Windows
#include "IBleServer.h"
#include "BleUtilities.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <mutex>
#include <queue>
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
        };

        struct ServiceData {
            GattServiceProvider provider = nullptr;
            ServiceConfig config;
            std::map<std::string, std::unique_ptr<CharacteristicData>> characteristics;
        };

        // Core members
        ServerConfig m_config;
        bool m_initialized = false;
        bool m_advertising = false;
        std::map<std::string, std::unique_ptr<ServiceData>> m_services;
        std::map<hstring, std::string> m_sessionToDeviceId;
        std::map<std::string, hstring> m_deviceToSessionId;

        // Callbacks
        ConnectionCallback m_connectionCallback;
        ErrorCallback m_errorCallback;

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

            // Stop advertising
            StopAdvertising();

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

            m_services.erase(it);
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

                service->characteristics.erase(charIt);
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
            std::lock_guard<std::mutex> lock(m_mutex);

            auto clients = characteristic.SubscribedClients();

            // Check for new connections
            for (const auto& client : clients) {
                auto sessionId = client.Session().DeviceId().Id();

                if (m_sessionToDeviceId.find(sessionId) == m_sessionToDeviceId.end()) {
                    // New device connected
                    std::wstring ws(sessionId);
                    std::string rawId(ws.begin(), ws.end());
                    std::string deviceId = Utils::GenerateDeviceId(rawId);
                    m_sessionToDeviceId[sessionId] = deviceId;
                    m_deviceToSessionId[deviceId] = sessionId;

                    if (m_connectionCallback) {
                        m_connectionCallback(deviceId, true);
                    }
                }
            }

            // Check for disconnections
            std::vector<hstring> toRemove;
            for (const auto& [sessionId, deviceId] : m_sessionToDeviceId) {
                bool found = false;
                for (const auto& client : clients) {
                    if (client.Session().DeviceId().Id() == sessionId) {
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    toRemove.push_back(sessionId);
                    m_deviceToSessionId.erase(deviceId);

                    if (m_connectionCallback) {
                        m_connectionCallback(deviceId, false);
                    }
                }
            }

            for (const auto& sessionId : toRemove) {
                m_sessionToDeviceId.erase(sessionId);
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

                    auto sessionId = args.Session().DeviceId().Id();
                    auto it = m_sessionToDeviceId.find(sessionId);
                    if (it != m_sessionToDeviceId.end()) {
                        deviceId = it->second;
                    }

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

            // Capture all necessary data while holding the mutex before co_await
            CharacteristicWriteCallback writeCallback;
            std::string deviceId;
            bool hasCallback = false;
            bool foundChar = false;
            CharacteristicData* charDataPtr = nullptr;

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto charData = GetCharacteristicData(serviceUuid, characteristicUuid);
                if (charData) {
                    foundChar = true;
                    writeCallback = charData->writeCallback;
                    hasCallback = (writeCallback != nullptr);
                    charDataPtr = charData;

                    auto sessionId = args.Session().DeviceId().Id();
                    auto it = m_sessionToDeviceId.find(sessionId);
                    if (it != m_sessionToDeviceId.end()) {
                        deviceId = it->second;
                    }
                }
            }

            auto request = co_await args.GetRequestAsync();

            try {
                if (!foundChar) {
                    if (request.Option() == GattWriteOption::WriteWithResponse) {
                        request.RespondWithProtocolError(GattProtocolError::InvalidHandle());
                    }
                    deferral.Complete();
                    co_return;
                }

                auto reader = DataReader::FromBuffer(request.Value());
                std::vector<uint8_t> data(reader.UnconsumedBufferLength());
                reader.ReadBytes(data);

                if (hasCallback && !deviceId.empty()) {
                    writeCallback(deviceId, data);
                }
                else {
                    // Update stored value
                    std::lock_guard<std::mutex> valueLock(charDataPtr->valueMutex);
                    charDataPtr->value = data;
                }

                if (request.Option() == GattWriteOption::WriteWithResponse) {
                    request.Respond();
                }
            }
            catch (...) {
                if (request.Option() == GattWriteOption::WriteWithResponse) {
                    request.RespondWithProtocolError(GattProtocolError::UnlikelyError());
                }
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