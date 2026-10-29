// BleServerLinux.cpp - Linux implementation using BlueZ and D-Bus
#include "IBleServer.h"
#include <glib.h>
#include <gio/gio.h>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <set>

namespace BleServer {

    // BlueZ D-Bus interfaces
    const char* BLUEZ_SERVICE = "org.bluez";
    const char* ADAPTER_INTERFACE = "org.bluez.Adapter1";
    const char* DEVICE_INTERFACE = "org.bluez.Device1";
    const char* GATT_MANAGER_INTERFACE = "org.bluez.GattManager1";
    const char* GATT_SERVICE_INTERFACE = "org.bluez.GattService1";
    const char* GATT_CHARACTERISTIC_INTERFACE = "org.bluez.GattCharacteristic1";
    const char* LE_ADVERTISING_MANAGER_INTERFACE = "org.bluez.LEAdvertisingManager1";
    const char* LE_ADVERTISEMENT_INTERFACE = "org.bluez.LEAdvertisement1";
    const char* PROPERTIES_INTERFACE = "org.freedesktop.DBus.Properties";
    const char* OBJECT_MANAGER_INTERFACE = "org.freedesktop.DBus.ObjectManager";

    // D-Bus introspection XML for GATT Service
    const char* GATT_SERVICE_INTROSPECTION =
        "<node>"
        "  <interface name='org.bluez.GattService1'>"
        "    <property name='UUID' type='s' access='read'/>"
        "    <property name='Primary' type='b' access='read'/>"
        "  </interface>"
        "  <interface name='org.freedesktop.DBus.Properties'>"
        "    <method name='Get'>"
        "      <arg name='interface' type='s' direction='in'/>"
        "      <arg name='name' type='s' direction='in'/>"
        "      <arg name='value' type='v' direction='out'/>"
        "    </method>"
        "    <method name='GetAll'>"
        "      <arg name='interface' type='s' direction='in'/>"
        "      <arg name='properties' type='a{sv}' direction='out'/>"
        "    </method>"
        "    <method name='Set'>"
        "      <arg name='interface' type='s' direction='in'/>"
        "      <arg name='name' type='s' direction='in'/>"
        "      <arg name='value' type='v' direction='in'/>"
        "    </method>"
        "    <signal name='PropertiesChanged'>"
        "      <arg name='interface' type='s'/>"
        "      <arg name='changed_properties' type='a{sv}'/>"
        "      <arg name='invalidated_properties' type='as'/>"
        "    </signal>"
        "  </interface>"
        "</node>";

    // D-Bus introspection XML for GATT Characteristic
    const char* GATT_CHARACTERISTIC_INTROSPECTION =
        "<node>"
        "  <interface name='org.bluez.GattCharacteristic1'>"
        "    <method name='ReadValue'>"
        "      <arg name='options' type='a{sv}' direction='in'/>"
        "      <arg name='value' type='ay' direction='out'/>"
        "    </method>"
        "    <method name='WriteValue'>"
        "      <arg name='value' type='ay' direction='in'/>"
        "      <arg name='options' type='a{sv}' direction='in'/>"
        "    </method>"
        "    <method name='StartNotify'/>"
        "    <method name='StopNotify'/>"
        "    <property name='UUID' type='s' access='read'/>"
        "    <property name='Service' type='o' access='read'/>"
        "    <property name='Value' type='ay' access='read'/>"
        "    <property name='Notifying' type='b' access='read'/>"
        "    <property name='Flags' type='as' access='read'/>"
        "  </interface>"
        "  <interface name='org.freedesktop.DBus.Properties'>"
        "    <method name='Get'>"
        "      <arg name='interface' type='s' direction='in'/>"
        "      <arg name='name' type='s' direction='in'/>"
        "      <arg name='value' type='v' direction='out'/>"
        "    </method>"
        "    <method name='GetAll'>"
        "      <arg name='interface' type='s' direction='in'/>"
        "      <arg name='properties' type='a{sv}' direction='out'/>"
        "    </method>"
        "    <method name='Set'>"
        "      <arg name='interface' type='s' direction='in'/>"
        "      <arg name='name' type='s' direction='in'/>"
        "      <arg name='value' type='v' direction='in'/>"
        "    </method>"
        "    <signal name='PropertiesChanged'>"
        "      <arg name='interface' type='s'/>"
        "      <arg name='changed_properties' type='a{sv}'/>"
        "      <arg name='invalidated_properties' type='as'/>"
        "    </signal>"
        "  </interface>"
        "</node>";

    // D-Bus introspection XML for LE Advertisement
    const char* LE_ADVERTISEMENT_INTROSPECTION =
        "<node>"
        "  <interface name='org.bluez.LEAdvertisement1'>"
        "    <method name='Release'/>"
        "    <property name='Type' type='s' access='read'/>"
        "    <property name='ServiceUUIDs' type='as' access='read'/>"
        "    <property name='LocalName' type='s' access='read'/>"
        "    <property name='Includes' type='as' access='read'/>"
        "  </interface>"
        "  <interface name='org.freedesktop.DBus.Properties'>"
        "    <method name='Get'>"
        "      <arg name='interface' type='s' direction='in'/>"
        "      <arg name='name' type='s' direction='in'/>"
        "      <arg name='value' type='v' direction='out'/>"
        "    </method>"
        "    <method name='GetAll'>"
        "      <arg name='interface' type='s' direction='in'/>"
        "      <arg name='properties' type='a{sv}' direction='out'/>"
        "    </method>"
        "  </interface>"
        "</node>";

    // D-Bus introspection XML for Application (root object for GATT registration)
    const char* APPLICATION_INTROSPECTION =
        "<node>"
        "  <interface name='org.freedesktop.DBus.ObjectManager'>"
        "    <method name='GetManagedObjects'>"
        "      <arg name='objects' type='a{oa{sa{sv}}}' direction='out'/>"
        "    </method>"
        "  </interface>"
        "</node>";

    class BleServerLinux : public IBleServer {
    private:
        struct CharacteristicData {
            CharacteristicConfig config;
            CharacteristicReadCallback readCallback;
            CharacteristicWriteCallback writeCallback;
            std::vector<uint8_t> value;
            std::mutex valueMutex;
            GDBusNodeInfo* nodeInfo = nullptr;
            guint registrationId = 0;
            std::string objectPath;
            std::string serviceUuid;  // Parent service UUID
            bool notifying = false;
            std::set<std::string> subscribedDevices;  // Devices that subscribed to notifications
        };

        struct ServiceData {
            ServiceConfig config;
            std::map<std::string, std::unique_ptr<CharacteristicData>> characteristics;
            GDBusNodeInfo* nodeInfo = nullptr;
            guint registrationId = 0;
            std::string objectPath;
        };

        ServerConfig m_config;
        bool m_initialized = false;
        bool m_advertising = false;
        std::map<std::string, std::unique_ptr<ServiceData>> m_services;
        ConnectionCallback m_connectionCallback;
        ErrorCallback m_errorCallback;
        mutable std::mutex m_mutex;

        GDBusConnection* m_connection = nullptr;
        GMainLoop* m_mainLoop = nullptr;
        std::thread m_dbusThread;
        std::atomic<bool> m_running{ false };
        std::string m_adapterPath;

        // D-Bus object paths
        std::string m_applicationPath = "/com/example/ble";
        guint m_applicationRegistrationId = 0;
        GDBusNodeInfo* m_applicationNodeInfo = nullptr;

        std::string m_advertisementPath = "/com/example/ble/advertisement";
        guint m_advertisementRegistrationId = 0;
        GDBusNodeInfo* m_advertisementNodeInfo = nullptr;

        // Connected devices tracking
        std::set<std::string> m_connectedDevices;
        guint m_deviceSignalId = 0;

        // Service index for generating unique paths
        int m_serviceIndex = 0;

    public:
        BleServerLinux() = default;

        ~BleServerLinux() {
            Shutdown();
        }

        BleError Initialize(const ServerConfig& config) override {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (m_initialized) {
                return BleError::AlreadyInitialized;
            }

            m_config = config;

            // Initialize D-Bus connection
            GError* error = nullptr;
            m_connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);

            if (!m_connection) {
                if (error) {
                    ReportError(BleError::Unknown, error->message);
                    g_error_free(error);
                }
                return BleError::Unknown;
            }

            // Find Bluetooth adapter
            if (!FindAdapter()) {
                ReportError(BleError::NotSupported, "No Bluetooth adapter found");
                return BleError::NotSupported;
            }

            // Start D-Bus main loop in separate thread
            m_running = true;
            m_mainLoop = g_main_loop_new(nullptr, FALSE);
            m_dbusThread = std::thread([this]() {
                g_main_loop_run(m_mainLoop);
                });

            m_initialized = true;
            return BleError::Success;
        }

        void Shutdown() override {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (!m_initialized) {
                return;
            }

            StopAdvertising();

            // Unregister GATT application
            UnregisterApplication();

            // Clean up services
            for (auto& [uuid, service] : m_services) {
                UnregisterService(service.get());
            }
            m_services.clear();

            // Remove device connection monitoring
            if (m_deviceSignalId != 0 && m_connection) {
                g_dbus_connection_signal_unsubscribe(m_connection, m_deviceSignalId);
                m_deviceSignalId = 0;
            }

            // Stop D-Bus main loop
            if (m_running) {
                m_running = false;
                if (m_mainLoop) {
                    g_main_loop_quit(m_mainLoop);
                }
                if (m_dbusThread.joinable()) {
                    m_dbusThread.join();
                }
            }

            // Clean up D-Bus node info
            if (m_applicationNodeInfo) {
                g_dbus_node_info_unref(m_applicationNodeInfo);
                m_applicationNodeInfo = nullptr;
            }

            if (m_advertisementNodeInfo) {
                g_dbus_node_info_unref(m_advertisementNodeInfo);
                m_advertisementNodeInfo = nullptr;
            }

            // Clean up D-Bus connection
            if (m_connection) {
                g_object_unref(m_connection);
                m_connection = nullptr;
            }

            if (m_mainLoop) {
                g_main_loop_unref(m_mainLoop);
                m_mainLoop = nullptr;
            }

            m_initialized = false;
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

            auto serviceData = std::make_unique<ServiceData>();
            serviceData->config = config;

            // Register service with BlueZ
            if (!RegisterService(serviceData.get())) {
                return BleError::Unknown;
            }

            m_services[config.uuid] = std::move(serviceData);
            return BleError::Success;
        }

        BleError RemoveService(const std::string& serviceUuid) override {
            std::lock_guard<std::mutex> lock(m_mutex);

            auto it = m_services.find(serviceUuid);
            if (it == m_services.end()) {
                return BleError::ServiceNotFound;
            }

            UnregisterService(it->second.get());
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

            auto charData = std::make_unique<CharacteristicData>();
            charData->config = config;
            charData->value = config.initialValue;

            // Register characteristic with BlueZ
            if (!RegisterCharacteristic(serviceUuid, charData.get())) {
                return BleError::Unknown;
            }

            service->characteristics[config.uuid] = std::move(charData);
            return BleError::Success;
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

            // Unregister from D-Bus
            auto& charData = charIt->second;
            if (charData->registrationId != 0 && m_connection) {
                g_dbus_connection_unregister_object(m_connection, charData->registrationId);
            }

            // Free node info
            if (charData->nodeInfo) {
                g_dbus_node_info_unref(charData->nodeInfo);
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

            // Create advertisement
            if (!CreateAdvertisement()) {
                return BleError::AdvertisingFailed;
            }

            m_advertising = true;
            return BleError::Success;
        }

        BleError StopAdvertising() override {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (!m_advertising) {
                return BleError::Success;
            }

            // Remove advertisement
            RemoveAdvertisement();

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
            return std::vector<std::string>(m_connectedDevices.begin(), m_connectedDevices.end());
        }

        BleError DisconnectDevice(const std::string& deviceId) override {
            // In BLE peripheral mode, we can't forcefully disconnect centrals
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

            // Emit PropertiesChanged signal
            EmitPropertiesChanged(serviceUuid, characteristicUuid, charData);

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

        // Helper to convert UUID to object path component
        std::string UuidToPath(const std::string& uuid) {
            std::string path = uuid;
            std::replace(path.begin(), path.end(), '-', '_');
            std::transform(path.begin(), path.end(), path.begin(), ::tolower);
            return path;
        }

        // Helper to convert property flags to BlueZ flag strings
        static std::vector<std::string> GetCharacteristicFlags(uint32_t properties, uint32_t permissions) {
            std::vector<std::string> flags;

            if (properties & static_cast<uint32_t>(CharacteristicProperty::Read)) {
                flags.push_back("read");
            }
            if (properties & static_cast<uint32_t>(CharacteristicProperty::Write)) {
                flags.push_back("write");
            }
            if (properties & static_cast<uint32_t>(CharacteristicProperty::WriteWithoutResponse)) {
                flags.push_back("write-without-response");
            }
            if (properties & static_cast<uint32_t>(CharacteristicProperty::Notify)) {
                flags.push_back("notify");
            }
            if (properties & static_cast<uint32_t>(CharacteristicProperty::Indicate)) {
                flags.push_back("indicate");
            }
            if (permissions & static_cast<uint32_t>(CharacteristicPermission::ReadEncrypted)) {
                flags.push_back("encrypt-read");
            }
            if (permissions & static_cast<uint32_t>(CharacteristicPermission::WriteEncrypted)) {
                flags.push_back("encrypt-write");
            }

            return flags;
        }

        bool FindAdapter() {
            // Get the default Bluetooth adapter
            GVariant* result = g_dbus_connection_call_sync(
                m_connection,
                BLUEZ_SERVICE,
                "/",
                "org.freedesktop.DBus.ObjectManager",
                "GetManagedObjects",
                nullptr,
                G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
                G_DBUS_CALL_FLAGS_NONE,
                -1,
                nullptr,
                nullptr
            );

            if (!result) {
                return false;
            }

            GVariantIter* objectIter;
            const gchar* objectPath;
            GVariant* interfaces;

            g_variant_get(result, "(a{oa{sa{sv}}})", &objectIter);

            while (g_variant_iter_loop(objectIter, "{&o@a{sa{sv}}}", &objectPath, &interfaces)) {
                if (g_variant_lookup(interfaces, ADAPTER_INTERFACE, "*")) {
                    m_adapterPath = objectPath;
                    g_variant_unref(interfaces);
                    break;
                }
            }

            g_variant_iter_free(objectIter);
            g_variant_unref(result);

            if (!m_adapterPath.empty()) {
                // Set up device connection monitoring
                SetupDeviceMonitoring();
            }

            return !m_adapterPath.empty();
        }

        void SetupDeviceMonitoring() {
            // Monitor InterfacesAdded and InterfacesRemoved signals for device connections
            // Subscribe to PropertiesChanged on Device1 interface
            m_deviceSignalId = g_dbus_connection_signal_subscribe(
                m_connection,
                BLUEZ_SERVICE,
                PROPERTIES_INTERFACE,
                "PropertiesChanged",
                nullptr,  // Any object path
                DEVICE_INTERFACE,
                G_DBUS_SIGNAL_FLAGS_NONE,
                OnDevicePropertiesChanged,
                this,
                nullptr
            );
        }

        static void OnDevicePropertiesChanged(
            GDBusConnection* connection,
            const gchar* sender_name,
            const gchar* object_path,
            const gchar* interface_name,
            const gchar* signal_name,
            GVariant* parameters,
            gpointer user_data)
        {
            auto* server = static_cast<BleServerLinux*>(user_data);

            const gchar* iface;
            GVariant* changed_properties;
            GVariant* invalidated_properties;

            g_variant_get(parameters, "(&s@a{sv}@as)", &iface, &changed_properties, &invalidated_properties);

            // Check if Connected property changed
            GVariant* connected_value = g_variant_lookup_value(changed_properties, "Connected", G_VARIANT_TYPE_BOOLEAN);
            if (connected_value) {
                gboolean connected = g_variant_get_boolean(connected_value);
                server->HandleDeviceConnection(object_path, connected);
                g_variant_unref(connected_value);
            }

            g_variant_unref(changed_properties);
            g_variant_unref(invalidated_properties);
        }

        void HandleDeviceConnection(const std::string& devicePath, bool connected) {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (connected) {
                m_connectedDevices.insert(devicePath);
            } else {
                m_connectedDevices.erase(devicePath);
            }

            if (m_connectionCallback) {
                m_connectionCallback(devicePath, connected);
            }
        }

        bool RegisterService(ServiceData* serviceData) {
            if (!m_connection || !serviceData) {
                return false;
            }

            // Generate object path for service
            serviceData->objectPath = m_applicationPath + "/service" + std::to_string(m_serviceIndex++);

            // Parse introspection data
            GError* error = nullptr;
            serviceData->nodeInfo = g_dbus_node_info_new_for_xml(GATT_SERVICE_INTROSPECTION, &error);
            if (error) {
                ReportError(BleError::Unknown, std::string("Failed to parse service introspection: ") + error->message);
                g_error_free(error);
                return false;
            }

            // Create vtable for service
            static const GDBusInterfaceVTable service_vtable = {
                OnServiceMethodCall,
                OnServiceGetProperty,
                nullptr,  // SetProperty not needed
                { 0 }
            };

            // Register service object
            serviceData->registrationId = g_dbus_connection_register_object(
                m_connection,
                serviceData->objectPath.c_str(),
                serviceData->nodeInfo->interfaces[0],
                &service_vtable,
                serviceData,
                nullptr,
                &error
            );

            if (error) {
                ReportError(BleError::Unknown, std::string("Failed to register service: ") + error->message);
                g_dbus_node_info_unref(serviceData->nodeInfo);
                serviceData->nodeInfo = nullptr;
                g_error_free(error);
                return false;
            }

            // Register Properties interface for the service
            static const GDBusInterfaceVTable properties_vtable = {
                OnPropertiesMethodCall,
                nullptr,
                nullptr,
                { 0 }
            };

            g_dbus_connection_register_object(
                m_connection,
                serviceData->objectPath.c_str(),
                serviceData->nodeInfo->interfaces[1],  // Properties interface
                &properties_vtable,
                serviceData,
                nullptr,
                nullptr
            );

            return true;
        }

        void UnregisterService(ServiceData* serviceData) {
            if (!serviceData || !m_connection) {
                return;
            }

            // Unregister all characteristics first
            for (auto& [uuid, charData] : serviceData->characteristics) {
                if (charData->registrationId != 0) {
                    g_dbus_connection_unregister_object(m_connection, charData->registrationId);
                }
                if (charData->nodeInfo) {
                    g_dbus_node_info_unref(charData->nodeInfo);
                }
            }
            serviceData->characteristics.clear();

            // Unregister service
            if (serviceData->registrationId != 0) {
                g_dbus_connection_unregister_object(m_connection, serviceData->registrationId);
            }

            if (serviceData->nodeInfo) {
                g_dbus_node_info_unref(serviceData->nodeInfo);
                serviceData->nodeInfo = nullptr;
            }
        }

        bool RegisterCharacteristic(const std::string& serviceUuid, CharacteristicData* charData) {
            if (!m_connection || !charData) {
                return false;
            }

            auto serviceIt = m_services.find(serviceUuid);
            if (serviceIt == m_services.end()) {
                return false;
            }

            auto& service = serviceIt->second;
            charData->serviceUuid = serviceUuid;

            // Generate object path for characteristic
            charData->objectPath = service->objectPath + "/char" + UuidToPath(charData->config.uuid);

            // Parse introspection data
            GError* error = nullptr;
            charData->nodeInfo = g_dbus_node_info_new_for_xml(GATT_CHARACTERISTIC_INTROSPECTION, &error);
            if (error) {
                ReportError(BleError::Unknown, std::string("Failed to parse characteristic introspection: ") + error->message);
                g_error_free(error);
                return false;
            }

            // Create vtable for characteristic
            static const GDBusInterfaceVTable char_vtable = {
                OnCharacteristicMethodCall,
                OnCharacteristicGetProperty,
                nullptr,
                { 0 }
            };

            // Register characteristic object
            charData->registrationId = g_dbus_connection_register_object(
                m_connection,
                charData->objectPath.c_str(),
                charData->nodeInfo->interfaces[0],
                &char_vtable,
                charData,
                nullptr,
                &error
            );

            if (error) {
                ReportError(BleError::Unknown, std::string("Failed to register characteristic: ") + error->message);
                g_dbus_node_info_unref(charData->nodeInfo);
                charData->nodeInfo = nullptr;
                g_error_free(error);
                return false;
            }

            // Register Properties interface for the characteristic
            static const GDBusInterfaceVTable properties_vtable = {
                OnPropertiesMethodCall,
                nullptr,
                nullptr,
                { 0 }
            };

            g_dbus_connection_register_object(
                m_connection,
                charData->objectPath.c_str(),
                charData->nodeInfo->interfaces[1],  // Properties interface
                &properties_vtable,
                charData,
                nullptr,
                nullptr
            );

            return true;
        }

        bool RegisterApplication() {
            if (!m_connection) {
                return false;
            }

            // Register the application root object with ObjectManager
            GError* error = nullptr;
            m_applicationNodeInfo = g_dbus_node_info_new_for_xml(APPLICATION_INTROSPECTION, &error);
            if (error) {
                ReportError(BleError::Unknown, std::string("Failed to parse application introspection: ") + error->message);
                g_error_free(error);
                return false;
            }

            static const GDBusInterfaceVTable app_vtable = {
                OnApplicationMethodCall,
                nullptr,
                nullptr,
                { 0 }
            };

            m_applicationRegistrationId = g_dbus_connection_register_object(
                m_connection,
                m_applicationPath.c_str(),
                m_applicationNodeInfo->interfaces[0],
                &app_vtable,
                this,
                nullptr,
                &error
            );

            if (error) {
                ReportError(BleError::Unknown, std::string("Failed to register application: ") + error->message);
                g_dbus_node_info_unref(m_applicationNodeInfo);
                m_applicationNodeInfo = nullptr;
                g_error_free(error);
                return false;
            }

            // Call RegisterApplication on GattManager1
            GVariantBuilder options_builder;
            g_variant_builder_init(&options_builder, G_VARIANT_TYPE("a{sv}"));

            GVariant* result = g_dbus_connection_call_sync(
                m_connection,
                BLUEZ_SERVICE,
                m_adapterPath.c_str(),
                GATT_MANAGER_INTERFACE,
                "RegisterApplication",
                g_variant_new("(oa{sv})", m_applicationPath.c_str(), &options_builder),
                nullptr,
                G_DBUS_CALL_FLAGS_NONE,
                -1,
                nullptr,
                &error
            );

            if (error) {
                ReportError(BleError::Unknown, std::string("Failed to register GATT application: ") + error->message);
                g_error_free(error);
                return false;
            }

            if (result) {
                g_variant_unref(result);
            }

            return true;
        }

        void UnregisterApplication() {
            if (!m_connection || m_applicationRegistrationId == 0) {
                return;
            }

            // Call UnregisterApplication on GattManager1
            GError* error = nullptr;
            GVariant* result = g_dbus_connection_call_sync(
                m_connection,
                BLUEZ_SERVICE,
                m_adapterPath.c_str(),
                GATT_MANAGER_INTERFACE,
                "UnregisterApplication",
                g_variant_new("(o)", m_applicationPath.c_str()),
                nullptr,
                G_DBUS_CALL_FLAGS_NONE,
                -1,
                nullptr,
                &error
            );

            if (error) {
                g_error_free(error);
            } else if (result) {
                g_variant_unref(result);
            }

            // Unregister application object
            if (m_applicationRegistrationId != 0) {
                g_dbus_connection_unregister_object(m_connection, m_applicationRegistrationId);
                m_applicationRegistrationId = 0;
            }
        }

        bool CreateAdvertisement() {
            if (!m_connection) {
                return false;
            }

            // Parse introspection data
            GError* error = nullptr;
            m_advertisementNodeInfo = g_dbus_node_info_new_for_xml(LE_ADVERTISEMENT_INTROSPECTION, &error);
            if (error) {
                ReportError(BleError::AdvertisingFailed, std::string("Failed to parse advertisement introspection: ") + error->message);
                g_error_free(error);
                return false;
            }

            // Create vtable for advertisement
            static const GDBusInterfaceVTable adv_vtable = {
                OnAdvertisementMethodCall,
                OnAdvertisementGetProperty,
                nullptr,
                { 0 }
            };

            // Register advertisement object
            m_advertisementRegistrationId = g_dbus_connection_register_object(
                m_connection,
                m_advertisementPath.c_str(),
                m_advertisementNodeInfo->interfaces[0],
                &adv_vtable,
                this,
                nullptr,
                &error
            );

            if (error) {
                ReportError(BleError::AdvertisingFailed, std::string("Failed to register advertisement: ") + error->message);
                g_dbus_node_info_unref(m_advertisementNodeInfo);
                m_advertisementNodeInfo = nullptr;
                g_error_free(error);
                return false;
            }

            // Register Properties interface for advertisement
            static const GDBusInterfaceVTable properties_vtable = {
                OnPropertiesMethodCall,
                nullptr,
                nullptr,
                { 0 }
            };

            g_dbus_connection_register_object(
                m_connection,
                m_advertisementPath.c_str(),
                m_advertisementNodeInfo->interfaces[1],  // Properties interface
                &properties_vtable,
                this,
                nullptr,
                nullptr
            );

            // Call RegisterAdvertisement on LEAdvertisingManager1
            GVariantBuilder options_builder;
            g_variant_builder_init(&options_builder, G_VARIANT_TYPE("a{sv}"));

            GVariant* result = g_dbus_connection_call_sync(
                m_connection,
                BLUEZ_SERVICE,
                m_adapterPath.c_str(),
                LE_ADVERTISING_MANAGER_INTERFACE,
                "RegisterAdvertisement",
                g_variant_new("(oa{sv})", m_advertisementPath.c_str(), &options_builder),
                nullptr,
                G_DBUS_CALL_FLAGS_NONE,
                -1,
                nullptr,
                &error
            );

            if (error) {
                ReportError(BleError::AdvertisingFailed, std::string("Failed to register advertisement: ") + error->message);
                g_error_free(error);
                return false;
            }

            if (result) {
                g_variant_unref(result);
            }

            // Also need to register GATT application before advertising
            if (m_applicationRegistrationId == 0 && !m_services.empty()) {
                RegisterApplication();
            }

            return true;
        }

        void RemoveAdvertisement() {
            if (!m_connection || m_advertisementRegistrationId == 0) {
                return;
            }

            // Call UnregisterAdvertisement on LEAdvertisingManager1
            GError* error = nullptr;
            GVariant* result = g_dbus_connection_call_sync(
                m_connection,
                BLUEZ_SERVICE,
                m_adapterPath.c_str(),
                LE_ADVERTISING_MANAGER_INTERFACE,
                "UnregisterAdvertisement",
                g_variant_new("(o)", m_advertisementPath.c_str()),
                nullptr,
                G_DBUS_CALL_FLAGS_NONE,
                -1,
                nullptr,
                &error
            );

            if (error) {
                g_error_free(error);
            } else if (result) {
                g_variant_unref(result);
            }

            // Unregister advertisement object
            if (m_advertisementRegistrationId != 0) {
                g_dbus_connection_unregister_object(m_connection, m_advertisementRegistrationId);
                m_advertisementRegistrationId = 0;
            }

            if (m_advertisementNodeInfo) {
                g_dbus_node_info_unref(m_advertisementNodeInfo);
                m_advertisementNodeInfo = nullptr;
            }
        }

        void EmitPropertiesChanged(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            CharacteristicData* charData) {
            if (!m_connection || !charData) {
                return;
            }

            // Build the changed properties variant
            GVariantBuilder builder;
            g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));

            // Add Value property
            {
                std::lock_guard<std::mutex> valueLock(charData->valueMutex);
                GVariantBuilder value_builder;
                g_variant_builder_init(&value_builder, G_VARIANT_TYPE("ay"));
                for (uint8_t byte : charData->value) {
                    g_variant_builder_add(&value_builder, "y", byte);
                }
                g_variant_builder_add(&builder, "{sv}", "Value", g_variant_builder_end(&value_builder));
            }

            GVariantBuilder invalidated_builder;
            g_variant_builder_init(&invalidated_builder, G_VARIANT_TYPE("as"));

            // Emit PropertiesChanged signal
            GError* error = nullptr;
            g_dbus_connection_emit_signal(
                m_connection,
                nullptr,  // Broadcast to all
                charData->objectPath.c_str(),
                PROPERTIES_INTERFACE,
                "PropertiesChanged",
                g_variant_new("(sa{sv}as)",
                    GATT_CHARACTERISTIC_INTERFACE,
                    &builder,
                    &invalidated_builder),
                &error
            );

            if (error) {
                ReportError(BleError::Unknown, std::string("Failed to emit PropertiesChanged: ") + error->message);
                g_error_free(error);
            }
        }

        // ========== D-Bus Callback Handlers ==========

        // Service property getter
        static GVariant* OnServiceGetProperty(
            GDBusConnection* connection,
            const gchar* sender,
            const gchar* object_path,
            const gchar* interface_name,
            const gchar* property_name,
            GError** error,
            gpointer user_data)
        {
            auto* serviceData = static_cast<ServiceData*>(user_data);

            if (g_strcmp0(property_name, "UUID") == 0) {
                return g_variant_new_string(serviceData->config.uuid.c_str());
            }
            else if (g_strcmp0(property_name, "Primary") == 0) {
                return g_variant_new_boolean(serviceData->config.isPrimary);
            }

            g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "Property not supported");
            return nullptr;
        }

        static void OnServiceMethodCall(
            GDBusConnection* connection,
            const gchar* sender,
            const gchar* object_path,
            const gchar* interface_name,
            const gchar* method_name,
            GVariant* parameters,
            GDBusMethodInvocation* invocation,
            gpointer user_data)
        {
            // GATT Service doesn't have methods
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
                "Method not supported");
        }

        // Characteristic property getter
        static GVariant* OnCharacteristicGetProperty(
            GDBusConnection* connection,
            const gchar* sender,
            const gchar* object_path,
            const gchar* interface_name,
            const gchar* property_name,
            GError** error,
            gpointer user_data)
        {
            auto* charData = static_cast<CharacteristicData*>(user_data);

            if (g_strcmp0(property_name, "UUID") == 0) {
                return g_variant_new_string(charData->config.uuid.c_str());
            }
            else if (g_strcmp0(property_name, "Service") == 0) {
                // Return parent service object path
                return g_variant_new_object_path(charData->objectPath.substr(0, charData->objectPath.find_last_of('/')).c_str());
            }
            else if (g_strcmp0(property_name, "Value") == 0) {
                std::lock_guard<std::mutex> valueLock(charData->valueMutex);
                GVariantBuilder builder;
                g_variant_builder_init(&builder, G_VARIANT_TYPE("ay"));
                for (uint8_t byte : charData->value) {
                    g_variant_builder_add(&builder, "y", byte);
                }
                return g_variant_builder_end(&builder);
            }
            else if (g_strcmp0(property_name, "Notifying") == 0) {
                return g_variant_new_boolean(charData->notifying);
            }
            else if (g_strcmp0(property_name, "Flags") == 0) {
                GVariantBuilder builder;
                g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));

                for (const auto& flag : GetCharacteristicFlags(charData->config.properties, charData->config.permissions)) {
                    g_variant_builder_add(&builder, "s", flag.c_str());
                }

                return g_variant_builder_end(&builder);
            }

            g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "Property not supported");
            return nullptr;
        }

        static void OnCharacteristicMethodCall(
            GDBusConnection* connection,
            const gchar* sender,
            const gchar* object_path,
            const gchar* interface_name,
            const gchar* method_name,
            GVariant* parameters,
            GDBusMethodInvocation* invocation,
            gpointer user_data)
        {
            auto* charData = static_cast<CharacteristicData*>(user_data);

            if (g_strcmp0(method_name, "ReadValue") == 0) {
                // Extract device path from options if available
                GVariant* options;
                g_variant_get(parameters, "(@a{sv})", &options);

                std::string deviceId = sender;  // Use sender as device ID
                GVariant* device_variant = g_variant_lookup_value(options, "device", G_VARIANT_TYPE_OBJECT_PATH);
                if (device_variant) {
                    deviceId = g_variant_get_string(device_variant, nullptr);
                    g_variant_unref(device_variant);
                }
                g_variant_unref(options);

                // Call read callback if set
                std::vector<uint8_t> value;
                if (charData->readCallback) {
                    value = charData->readCallback(deviceId);
                    // Update stored value
                    std::lock_guard<std::mutex> valueLock(charData->valueMutex);
                    charData->value = value;
                } else {
                    std::lock_guard<std::mutex> valueLock(charData->valueMutex);
                    value = charData->value;
                }

                // Return value as byte array
                GVariantBuilder builder;
                g_variant_builder_init(&builder, G_VARIANT_TYPE("ay"));
                for (uint8_t byte : value) {
                    g_variant_builder_add(&builder, "y", byte);
                }

                g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ay)", g_variant_builder_end(&builder)));
            }
            else if (g_strcmp0(method_name, "WriteValue") == 0) {
                GVariant* value_variant;
                GVariant* options;
                g_variant_get(parameters, "(@ay@a{sv})", &value_variant, &options);

                // Extract device path from options if available
                std::string deviceId = sender;
                GVariant* device_variant = g_variant_lookup_value(options, "device", G_VARIANT_TYPE_OBJECT_PATH);
                if (device_variant) {
                    deviceId = g_variant_get_string(device_variant, nullptr);
                    g_variant_unref(device_variant);
                }

                // Check for offset in options (long writes via Prepare Write / Execute Write)
                uint32_t write_offset = 0;
                GVariant* offset_variant = g_variant_lookup_value(options, "Offset", G_VARIANT_TYPE_UINT32);
                if (offset_variant) {
                    write_offset = g_variant_get_uint32(offset_variant);
                    g_variant_unref(offset_variant);
                }
                g_variant_unref(options);

                // Extract byte array
                std::vector<uint8_t> data;
                GVariantIter iter;
                g_variant_iter_init(&iter, value_variant);
                guint8 byte_val;
                while (g_variant_iter_next(&iter, "y", &byte_val)) {
                    data.push_back(byte_val);
                }
                g_variant_unref(value_variant);

                // Update stored value (or write at offset when fragmented writes occur)
                {
                    std::lock_guard<std::mutex> valueLock(charData->valueMutex);
                    if (write_offset == 0) {
                        charData->value = data;
                    } else {
                        if (write_offset + data.size() > charData->value.size()) {
                            charData->value.resize(write_offset + data.size());
                        }
                        std::copy(data.begin(), data.end(), charData->value.begin() + write_offset);
                    }
                }

                // Call write callback if set
                if (charData->writeCallback) {
                    {
                        std::lock_guard<std::mutex> valueLock(charData->valueMutex);
                        auto assembled = charData->value;
                        charData->writeCallback(deviceId, assembled);
                    }
                }

                g_dbus_method_invocation_return_value(invocation, nullptr);
            }
            else if (g_strcmp0(method_name, "StartNotify") == 0) {
                charData->notifying = true;
                charData->subscribedDevices.insert(sender);
                g_dbus_method_invocation_return_value(invocation, nullptr);
            }
            else if (g_strcmp0(method_name, "StopNotify") == 0) {
                charData->subscribedDevices.erase(sender);
                if (charData->subscribedDevices.empty()) {
                    charData->notifying = false;
                }
                g_dbus_method_invocation_return_value(invocation, nullptr);
            }
            else {
                g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
                    "Method not supported");
            }
        }

        // Advertisement property getter
        static GVariant* OnAdvertisementGetProperty(
            GDBusConnection* connection,
            const gchar* sender,
            const gchar* object_path,
            const gchar* interface_name,
            const gchar* property_name,
            GError** error,
            gpointer user_data)
        {
            auto* server = static_cast<BleServerLinux*>(user_data);

            if (g_strcmp0(property_name, "Type") == 0) {
                return g_variant_new_string("peripheral");
            }
            else if (g_strcmp0(property_name, "ServiceUUIDs") == 0) {
                GVariantBuilder builder;
                g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));

                // Add all service UUIDs
                std::lock_guard<std::mutex> lock(server->m_mutex);
                for (const auto& [uuid, service] : server->m_services) {
                    g_variant_builder_add(&builder, "s", uuid.c_str());
                }

                return g_variant_builder_end(&builder);
            }
            else if (g_strcmp0(property_name, "LocalName") == 0) {
                std::lock_guard<std::mutex> lock(server->m_mutex);
                return g_variant_new_string(server->m_config.deviceName.c_str());
            }
            else if (g_strcmp0(property_name, "Includes") == 0) {
                GVariantBuilder builder;
                g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
                g_variant_builder_add(&builder, "s", "tx-power");
                return g_variant_builder_end(&builder);
            }

            g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "Property not supported");
            return nullptr;
        }

        static void OnAdvertisementMethodCall(
            GDBusConnection* connection,
            const gchar* sender,
            const gchar* object_path,
            const gchar* interface_name,
            const gchar* method_name,
            GVariant* parameters,
            GDBusMethodInvocation* invocation,
            gpointer user_data)
        {
            if (g_strcmp0(method_name, "Release") == 0) {
                // Advertisement was released by BlueZ
                auto* server = static_cast<BleServerLinux*>(user_data);
                server->m_advertising = false;
                g_dbus_method_invocation_return_value(invocation, nullptr);
            }
            else {
                g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
                    "Method not supported");
            }
        }

        // Application ObjectManager GetManagedObjects
        static void OnApplicationMethodCall(
            GDBusConnection* connection,
            const gchar* sender,
            const gchar* object_path,
            const gchar* interface_name,
            const gchar* method_name,
            GVariant* parameters,
            GDBusMethodInvocation* invocation,
            gpointer user_data)
        {
            auto* server = static_cast<BleServerLinux*>(user_data);

            if (g_strcmp0(method_name, "GetManagedObjects") == 0) {
                GVariantBuilder root_builder;
                g_variant_builder_init(&root_builder, G_VARIANT_TYPE("a{oa{sa{sv}}}"));

                std::lock_guard<std::mutex> lock(server->m_mutex);

                // Add all services and their characteristics
                for (const auto& [serviceUuid, service] : server->m_services) {
                    // Add service
                    GVariantBuilder iface_builder;
                    g_variant_builder_init(&iface_builder, G_VARIANT_TYPE("a{sa{sv}}"));

                    // Service interface
                    GVariantBuilder service_props_builder;
                    g_variant_builder_init(&service_props_builder, G_VARIANT_TYPE("a{sv}"));
                    g_variant_builder_add(&service_props_builder, "{sv}", "UUID", g_variant_new_string(service->config.uuid.c_str()));
                    g_variant_builder_add(&service_props_builder, "{sv}", "Primary", g_variant_new_boolean(service->config.isPrimary));
                    g_variant_builder_add(&iface_builder, "{sa{sv}}", GATT_SERVICE_INTERFACE, &service_props_builder);

                    g_variant_builder_add(&root_builder, "{oa{sa{sv}}}", service->objectPath.c_str(), &iface_builder);

                    // Add characteristics
                    for (const auto& [charUuid, characteristic] : service->characteristics) {
                        GVariantBuilder char_iface_builder;
                        g_variant_builder_init(&char_iface_builder, G_VARIANT_TYPE("a{sa{sv}}"));

                        // Characteristic interface
                        GVariantBuilder char_props_builder;
                        g_variant_builder_init(&char_props_builder, G_VARIANT_TYPE("a{sv}"));
                        g_variant_builder_add(&char_props_builder, "{sv}", "UUID", g_variant_new_string(characteristic->config.uuid.c_str()));
                        g_variant_builder_add(&char_props_builder, "{sv}", "Service", g_variant_new_object_path(service->objectPath.c_str()));

                        // Add Value
                        {
                            std::lock_guard<std::mutex> valueLock(characteristic->valueMutex);
                            GVariantBuilder value_builder;
                            g_variant_builder_init(&value_builder, G_VARIANT_TYPE("ay"));
                            for (uint8_t byte : characteristic->value) {
                                g_variant_builder_add(&value_builder, "y", byte);
                            }
                            g_variant_builder_add(&char_props_builder, "{sv}", "Value", g_variant_builder_end(&value_builder));
                        }

                        g_variant_builder_add(&char_props_builder, "{sv}", "Notifying", g_variant_new_boolean(characteristic->notifying));

                        // Add Flags
                        GVariantBuilder flags_builder;
                        g_variant_builder_init(&flags_builder, G_VARIANT_TYPE("as"));
                        for (const auto& flag : GetCharacteristicFlags(characteristic->config.properties, characteristic->config.permissions)) {
                            g_variant_builder_add(&flags_builder, "s", flag.c_str());
                        }
                        g_variant_builder_add(&char_props_builder, "{sv}", "Flags", g_variant_builder_end(&flags_builder));

                        g_variant_builder_add(&char_iface_builder, "{sa{sv}}", GATT_CHARACTERISTIC_INTERFACE, &char_props_builder);

                        g_variant_builder_add(&root_builder, "{oa{sa{sv}}}", characteristic->objectPath.c_str(), &char_iface_builder);
                    }
                }

                g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a{oa{sa{sv}}})", g_variant_builder_end(&root_builder)));
            }
            else {
                g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
                    "Method not supported");
            }
        }

        // Generic Properties interface handler
        static void OnPropertiesMethodCall(
            GDBusConnection* connection,
            const gchar* sender,
            const gchar* object_path,
            const gchar* interface_name,
            const gchar* method_name,
            GVariant* parameters,
            GDBusMethodInvocation* invocation,
            gpointer user_data)
        {
            if (g_strcmp0(method_name, "Get") == 0) {
                const gchar* iface;
                const gchar* property;
                g_variant_get(parameters, "(&s&s)", &iface, &property);

                GError* error = nullptr;
                GVariant* value = nullptr;
                if (g_strcmp0(iface, GATT_SERVICE_INTERFACE) == 0) {
                    value = OnServiceGetProperty(connection, sender, object_path, iface, property, &error, user_data);
                }
                else if (g_strcmp0(iface, GATT_CHARACTERISTIC_INTERFACE) == 0) {
                    value = OnCharacteristicGetProperty(connection, sender, object_path, iface, property, &error, user_data);
                }
                else if (g_strcmp0(iface, LE_ADVERTISEMENT_INTERFACE) == 0) {
                    value = OnAdvertisementGetProperty(connection, sender, object_path, iface, property, &error, user_data);
                }

                if (value) {
                    g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", value));
                }
                else {
                    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
                        error ? error->message : "Property not supported");
                }
                if (error) {
                    g_error_free(error);
                }
            }
            else if (g_strcmp0(method_name, "GetAll") == 0) {
                const gchar* iface;
                g_variant_get(parameters, "(&s)", &iface);

                GVariantBuilder builder;
                g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));

                if (g_strcmp0(iface, GATT_SERVICE_INTERFACE) == 0) {
                    auto* serviceData = static_cast<ServiceData*>(user_data);
                    g_variant_builder_add(&builder, "{sv}", "UUID", g_variant_new_string(serviceData->config.uuid.c_str()));
                    g_variant_builder_add(&builder, "{sv}", "Primary", g_variant_new_boolean(serviceData->config.isPrimary));
                }
                else if (g_strcmp0(iface, GATT_CHARACTERISTIC_INTERFACE) == 0) {
                    auto* charData = static_cast<CharacteristicData*>(user_data);
                    g_variant_builder_add(&builder, "{sv}", "UUID", g_variant_new_string(charData->config.uuid.c_str()));
                    g_variant_builder_add(&builder, "{sv}", "Service", g_variant_new_object_path(charData->objectPath.substr(0, charData->objectPath.find_last_of('/')).c_str()));

                    GVariantBuilder valueBuilder;
                    g_variant_builder_init(&valueBuilder, G_VARIANT_TYPE("ay"));
                    {
                        std::lock_guard<std::mutex> valueLock(charData->valueMutex);
                        for (uint8_t byte : charData->value) {
                            g_variant_builder_add(&valueBuilder, "y", byte);
                        }
                    }
                    g_variant_builder_add(&builder, "{sv}", "Value", g_variant_builder_end(&valueBuilder));
                    g_variant_builder_add(&builder, "{sv}", "Notifying", g_variant_new_boolean(charData->notifying));

                    GVariantBuilder flagsBuilder;
                    g_variant_builder_init(&flagsBuilder, G_VARIANT_TYPE("as"));
                    for (const auto& flag : GetCharacteristicFlags(charData->config.properties, charData->config.permissions)) {
                        g_variant_builder_add(&flagsBuilder, "s", flag.c_str());
                    }
                    g_variant_builder_add(&builder, "{sv}", "Flags", g_variant_builder_end(&flagsBuilder));
                }
                else if (g_strcmp0(iface, LE_ADVERTISEMENT_INTERFACE) == 0) {
                    auto* server = static_cast<BleServerLinux*>(user_data);
                    g_variant_builder_add(&builder, "{sv}", "Type", g_variant_new_string("peripheral"));
                    GVariantBuilder serviceUuidBuilder;
                    g_variant_builder_init(&serviceUuidBuilder, G_VARIANT_TYPE("as"));
                    for (const auto& [uuid, service] : server->m_services) {
                        g_variant_builder_add(&serviceUuidBuilder, "s", uuid.c_str());
                    }
                    g_variant_builder_add(&builder, "{sv}", "ServiceUUIDs", g_variant_builder_end(&serviceUuidBuilder));
                    g_variant_builder_add(&builder, "{sv}", "LocalName", g_variant_new_string(server->m_config.deviceName.c_str()));

                    GVariantBuilder includesBuilder;
                    g_variant_builder_init(&includesBuilder, G_VARIANT_TYPE("as"));
                    g_variant_builder_add(&includesBuilder, "s", "tx-power");
                    g_variant_builder_add(&builder, "{sv}", "Includes", g_variant_builder_end(&includesBuilder));
                }
                else {
                    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
                        "Interface not supported");
                    return;
                }

                g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a{sv})", g_variant_builder_end(&builder)));
            }
            else if (g_strcmp0(method_name, "Set") == 0) {
                g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_PROPERTY_READ_ONLY,
                    "Properties are read-only");
            }
            else {
                g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
                    "Method not supported");
            }
        }

        void ReportError(BleError error, const std::string& message) {
            if (m_errorCallback) {
                m_errorCallback(error, message);
            }
        }
    };

    // Platform-specific factory function
    std::unique_ptr<IBleServer> CreatePlatformServer() {
        return std::make_unique<BleServerLinux>();
    }

} // namespace BleServer