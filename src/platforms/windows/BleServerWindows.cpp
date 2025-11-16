// BleServerWindows.cpp - Clean BLE Server implementation for Windows
#include "IBleServer.h"
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

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;

namespace BleServer {

    class BleServerWindows : public IBleServer {
    private:
        struct CharacteristicData {
            GattLocalCharacteristic characteristic = nullptr;
            CharacteristicConfig config;
            CharacteristicReadCallback readCallback;
            CharacteristicWriteCallback writeCallback;
            std::vector<uint8_t> value;
            std::mutex valueMutex;
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
                ReportError(BleError::Unknown, winrt::to_string(ex.message()));
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

            if (m_services.find(config.uuid) != m_services.end()) {
                ReportError(BleError::InvalidParameter, "Service already exists");
                return BleError::InvalidParameter;
            }

            try {
                guid serviceGuid(config.uuid);
                auto providerResult = GattServiceProvider::CreateAsync(serviceGuid).get();

                if (providerResult.Error() != BluetoothError::Success) {
                    ReportError(BleError::Unknown, "Failed to create service provider");
                    return BleError::Unknown;
                }

                auto serviceData = std::make_unique<ServiceData>();
                serviceData->provider = providerResult.ServiceProvider();
                serviceData->config = config;

                m_services[config.uuid] = std::move(serviceData);
                return BleError::Success;
            }
            catch (const hresult_error& ex) {
                ReportError(BleError::Unknown, winrt::to_string(ex.message()));
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

            auto& service = serviceIt->second;
            if (service->characteristics.find(config.uuid) != service->characteristics.end()) {
                ReportError(BleError::InvalidParameter, "Characteristic already exists");
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
                    ReportError(BleError::Unknown, "Failed to create characteristic");
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
                ReportError(BleError::Unknown, winrt::to_string(ex.message()));
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

            service->characteristics.erase(charIt);
            return BleError::Success;
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
            charData->characteristic.SubscribedClientsChanged(
                [this, serviceUuid, characteristicUuid](GattLocalCharacteristic const& sender,
                    IInspectable const&) {
                        HandleSubscriptionChanged(serviceUuid, characteristicUuid, sender);
                });

            // Read request handler
            if (charData->config.properties & static_cast<uint32_t>(CharacteristicProperty::Read)) {
                charData->characteristic.ReadRequested(
                    [this, serviceUuid, characteristicUuid](GattLocalCharacteristic const& sender,
                        GattReadRequestedEventArgs const& args) {
                            HandleReadRequest(serviceUuid, characteristicUuid, args);
                    });
            }

            // Write request handler
            if (charData->config.properties & (static_cast<uint32_t>(CharacteristicProperty::Write) |
                static_cast<uint32_t>(CharacteristicProperty::WriteWithoutResponse))) {
                charData->characteristic.WriteRequested(
                    [this, serviceUuid, characteristicUuid](GattLocalCharacteristic const& sender,
                        GattWriteRequestedEventArgs const& args) {
                            HandleWriteRequest(serviceUuid, characteristicUuid, args);
                    });
            }
        }

        void HandleSubscriptionChanged(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            GattLocalCharacteristic const& characteristic) {
            auto clients = characteristic.SubscribedClients();

            // Check for new connections
            for (const auto& client : clients) {
                auto sessionId = client.Session().DeviceId().Id();

                if (m_sessionToDeviceId.find(sessionId) == m_sessionToDeviceId.end()) {
                    // New device connected
                    std::string deviceId = GenerateDeviceId(sessionId);
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
            auto request = co_await args.GetRequestAsync();

            try {
                auto charData = GetCharacteristicData(serviceUuid, characteristicUuid);
                if (!charData) {
                    request.RespondWithProtocolError(GattProtocolError::InvalidHandle());
                    deferral.Complete();
                    co_return;
                }

                std::vector<uint8_t> value;

                if (charData->readCallback) {
                    auto sessionId = args.Session().DeviceId().Id();
                    auto it = m_sessionToDeviceId.find(sessionId);
                    if (it != m_sessionToDeviceId.end()) {
                        value = charData->readCallback(it->second);
                    }
                }
                else {
                    std::lock_guard<std::mutex> valueLock(charData->valueMutex);
                    value = charData->value;
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
            auto request = co_await args.GetRequestAsync();

            try {
                auto charData = GetCharacteristicData(serviceUuid, characteristicUuid);
                if (!charData) {
                    if (request.Option() == GattWriteOption::WriteWithResponse) {
                        request.RespondWithProtocolError(GattProtocolError::InvalidHandle());
                    }
                    deferral.Complete();
                    co_return;
                }

                auto reader = DataReader::FromBuffer(request.Value());
                std::vector<uint8_t> data(reader.UnconsumedBufferLength());
                reader.ReadBytes(data);

                auto sessionId = args.Session().DeviceId().Id();
                auto it = m_sessionToDeviceId.find(sessionId);
                std::string deviceId = (it != m_sessionToDeviceId.end()) ? it->second : "";

                if (charData->writeCallback && !deviceId.empty()) {
                    charData->writeCallback(deviceId, data);
                }
                else {
                    std::lock_guard<std::mutex> valueLock(charData->valueMutex);
                    charData->value = data;
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

        std::string GenerateDeviceId(const hstring& sessionId) {
            // Extract meaningful part from session ID or generate a simple ID
            std::wstring ws(sessionId);
            std::string s(ws.begin(), ws.end());

            // Try to extract MAC address if present
            size_t pos = s.rfind('-');
            if (pos != std::string::npos && pos + 1 < s.length()) {
                std::string mac = s.substr(pos + 1);
                // Validate MAC format (XX:XX:XX:XX:XX:XX)
                if (mac.length() == 17 &&
                    mac[2] == ':' && mac[5] == ':' && mac[8] == ':' &&
                    mac[11] == ':' && mac[14] == ':') {
                    return mac;
                }
            }

            // Fallback to simple counter-based ID
            static std::atomic<int> deviceCounter{ 0 };
            return "Device-" + std::to_string(++deviceCounter);
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