#pragma once
#include "BleMessageChunker.h"
#include <map>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <cstdint>

// Cross-platform export macro
#ifdef _WIN32
#ifdef BLE_SERVER_EXPORTS
#define BLE_API __declspec(dllexport)
#else
#define BLE_API __declspec(dllimport)
#endif
#else
#define BLE_API __attribute__((visibility("default")))
#endif


namespace BleServer {

    // Forward declarations
    class IBleService;
    class IBleCharacteristic;

    // Enums
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

    enum class ServerCapability {
        Advertising = 0x00000001,
        Connections = 0x00000002,
        Read = 0x00000004,
        Write = 0x00000008,
        Notify = 0x00000010,
        Indicate = 0x00000020,
        Descriptors = 0x00000040,
        Pairing = 0x00000080,
        Bonding = 0x00000100,
        Encryption = 0x00000200,
        Disconnect = 0x00000400,
        Chunking = 0x00000800
    };

    // Callbacks
    using ConnectionCallback = std::function<void(const std::string& deviceId, bool connected)>;
    using CharacteristicReadCallback = std::function<std::vector<uint8_t>(const std::string& deviceId)>;
    using CharacteristicWriteCallback = std::function<void(const std::string& deviceId, const std::vector<uint8_t>& data)>;
    using ErrorCallback = std::function<void(BleError error, const std::string& message)>;

    // Configuration structures
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
        uint32_t properties = 0;  // Combination of CharacteristicProperty flags
        uint32_t permissions = 0;  // Combination of CharacteristicPermission flags
        std::vector<uint8_t> initialValue;
        size_t maxLength = 512;
    };

    struct DescriptorConfig {
        std::string uuid;
        uint32_t permissions = 0;  // Combination of CharacteristicPermission flags
        std::vector<uint8_t> initialValue;
    };

    // Main BLE Server interface
    class BLE_API IBleServer {
    public:
        virtual ~IBleServer() = default;

        // Lifecycle
        virtual BleError Initialize(const ServerConfig& config) = 0;
        virtual void Shutdown() = 0;
        virtual bool IsInitialized() const = 0;

        // Service management
        virtual BleError AddService(const ServiceConfig& config) = 0;
        virtual BleError RemoveService(const std::string& serviceUuid) = 0;
        virtual std::vector<std::string> GetServices() const = 0;

        // Characteristic management
        virtual BleError AddCharacteristic(const std::string& serviceUuid,
            const CharacteristicConfig& config) = 0;
        virtual BleError RemoveCharacteristic(const std::string& serviceUuid,
            const std::string& characteristicUuid) = 0;

        virtual BleError AddDescriptor(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            const DescriptorConfig& config) {
            (void)serviceUuid;
            (void)characteristicUuid;
            (void)config;
            return BleError::NotSupported;
        }

        // Characteristic callbacks
        virtual BleError SetCharacteristicReadCallback(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            CharacteristicReadCallback callback) = 0;
        virtual BleError SetCharacteristicWriteCallback(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            CharacteristicWriteCallback callback) = 0;

        // Advertising
        virtual BleError StartAdvertising() = 0;
        virtual BleError StopAdvertising() = 0;
        virtual bool IsAdvertising() const = 0;

        // Connection management
        virtual void SetConnectionCallback(ConnectionCallback callback) = 0;
        virtual std::vector<std::string> GetConnectedDevices() const = 0;
        virtual BleError DisconnectDevice(const std::string& deviceId) = 0;

        // Data operations
        virtual BleError UpdateCharacteristicValue(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            const std::vector<uint8_t>& value) = 0;
        virtual BleError NotifyCharacteristicChanged(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            const std::string& deviceId = "") = 0;

        // Error handling
        virtual void SetErrorCallback(ErrorCallback callback) = 0;

        // Diagnostics: optional human-readable platform status. Default is
        // empty so platforms can opt in without breaking the ABI.
        virtual std::string DescribeStatus() const {
            return std::string();
        }

        virtual uint32_t GetCapabilities() const {
            return static_cast<uint32_t>(ServerCapability::Advertising) |
                static_cast<uint32_t>(ServerCapability::Connections) |
                static_cast<uint32_t>(ServerCapability::Read) |
                static_cast<uint32_t>(ServerCapability::Write) |
                static_cast<uint32_t>(ServerCapability::Notify) |
                static_cast<uint32_t>(ServerCapability::Indicate) |
                static_cast<uint32_t>(ServerCapability::Chunking);
        }

        // Factory method
        static std::unique_ptr<IBleServer> Create();
    };

} // namespace BleServer

// C API for Unity integration
extern "C" {
    struct BleServerConfigC {
        uint32_t size;
        const char* deviceName;
        bool requiresPairing;
        bool requiresBonding;
        int maxConnections;
    };

    struct BleCharacteristicConfigC {
        uint32_t size;
        const char* uuid;
        uint32_t properties;
        uint32_t permissions;
        const uint8_t* initialValue;
        int initialValueLength;
        int maxLength;
    };

    struct BleDescriptorConfigC {
        uint32_t size;
        const char* uuid;
        uint32_t permissions;
        const uint8_t* initialValue;
        int initialValueLength;
    };

    // Server lifecycle
    BLE_API void* BleServer_Create();
    BLE_API void BleServer_Destroy(void* server);
    BLE_API int BleServer_Initialize(void* server, const char* deviceName, bool requiresPairing);
    BLE_API int BleServer_InitializeEx(void* server, const BleServerConfigC* config);
    BLE_API void BleServer_Shutdown(void* server);
    BLE_API bool BleServer_IsInitialized(void* server);
    BLE_API int BleServer_GetApiVersion();
    BLE_API const char* BleServer_GetBuildInfo();
    BLE_API const char* BleServer_GetLastError(void* server);
    BLE_API uint32_t BleServer_GetCapabilities(void* server);
    BLE_API int BleServer_GetStatus(void* server, char* buffer, int* bufferLength);
    BLE_API void BleServer_LogStatus(void* server);

    // Service management
    BLE_API int BleServer_AddService(void* server, const char* serviceUuid, bool isPrimary);
    BLE_API int BleServer_RemoveService(void* server, const char* serviceUuid);

    // Characteristic management
    BLE_API int BleServer_AddCharacteristic(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        uint32_t properties,
        uint32_t permissions);
    BLE_API int BleServer_AddCharacteristicEx(void* server,
        const char* serviceUuid,
        const BleCharacteristicConfigC* config);
    BLE_API int BleServer_RemoveCharacteristic(void* server,
        const char* serviceUuid,
        const char* characteristicUuid);
    BLE_API int BleServer_AddDescriptor(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        const BleDescriptorConfigC* config);

    // Callbacks (using function pointers for Unity interop)
    typedef void (*ConnectionCallbackFn)(const char* deviceId, bool connected);
    typedef void (*DataCallbackFn)(const char* deviceId, const uint8_t* data, int dataLength);
    typedef void (*ErrorCallbackFn)(int error, const char* message);
    typedef void (*ConnectionCallbackExFn)(const char* deviceId, bool connected, void* userData);
    typedef int (*ReadCallbackFn)(const char* deviceId, uint8_t* buffer, int bufferLength, void* userData);
    typedef void (*DataCallbackExFn)(const char* deviceId, const uint8_t* data, int dataLength, void* userData);
    typedef void (*ErrorCallbackExFn)(int error, const char* message, void* userData);

    BLE_API void BleServer_SetConnectionCallback(void* server, ConnectionCallbackFn callback);
    BLE_API void BleServer_SetConnectionCallbackEx(void* server, ConnectionCallbackExFn callback, void* userData);
    BLE_API void BleServer_SetErrorCallback(void* server, ErrorCallbackFn callback);
    BLE_API void BleServer_SetErrorCallbackEx(void* server, ErrorCallbackExFn callback, void* userData);
    BLE_API int BleServer_SetCharacteristicReadCallback(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        ReadCallbackFn callback,
        void* userData);
    BLE_API void BleServer_SetCharacteristicWriteCallback(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        DataCallbackFn callback);
    BLE_API int BleServer_SetCharacteristicWriteCallbackEx(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        DataCallbackExFn callback,
        void* userData);

    // Advertising
    BLE_API int BleServer_StartAdvertising(void* server);
    BLE_API int BleServer_StopAdvertising(void* server);
    BLE_API bool BleServer_IsAdvertising(void* server);

    // Data operations
    BLE_API int BleServer_UpdateCharacteristicValue(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        const uint8_t* value,
        int valueLength);
    BLE_API int BleServer_NotifyCharacteristicChanged(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        const char* deviceId);
    BLE_API int BleServer_GetConnectedDeviceCount(void* server);
    BLE_API int BleServer_GetConnectedDeviceId(void* server, int index, char* buffer, int* bufferLength);
    BLE_API int BleServer_DisconnectDevice(void* server, const char* deviceId);

    // Message chunking helpers
    BLE_API void* BleChunker_Create();
    BLE_API void BleChunker_Destroy(void* chunker);
    BLE_API int BleChunker_SendMessage(void* chunker, void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        const uint8_t* data,
        int dataLength);
    BLE_API bool BleChunker_ProcessReceivedData(void* chunker,
        const uint8_t* data,
        int dataLength,
        uint8_t* completeMessage,
        int* completeMessageLength);
    BLE_API int BleChunker_ProcessIncoming(void* chunker,
        const uint8_t* data,
        int dataLength,
        uint8_t* ackBuffer,
        int* ackLength);
    BLE_API int BleChunker_GetNextMessageSize(void* chunker);
    BLE_API bool BleChunker_ReadNextMessage(void* chunker,
        uint8_t* completeMessage,
        int* completeMessageLength);
    BLE_API int BleChunker_CleanupTimedOut(void* chunker, int timeoutMs);
}