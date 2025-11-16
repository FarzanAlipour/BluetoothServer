#pragma once
#include <map>
#include <string>
#include <vector>
#include <functional>
#include <memory>

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

        // Factory method
        static std::unique_ptr<IBleServer> Create();
    };

    // Message chunking utility
    class BLE_API BleMessageChunker {
    public:
        static constexpr size_t CHUNK_SIZE = 256;
        static constexpr size_t HEADER_SIZE = 13; // 1 (type) + 4 (messageId) + 4 (chunkIndex) + 4 (totalChunks)
        static constexpr size_t MAX_PAYLOAD_SIZE = CHUNK_SIZE - HEADER_SIZE;

        struct ChunkHeader {
            uint8_t type;        // 0x01 for data, 0x02 for ack
            uint32_t messageId;
            uint32_t chunkIndex;
            uint32_t totalChunks;
        };

        // Split message into chunks
        static std::vector<std::vector<uint8_t>> CreateChunks(const std::vector<uint8_t>& message);

        // Process received chunk
        static bool ProcessChunk(const std::vector<uint8_t>& chunk,
            std::map<uint32_t, std::vector<std::vector<uint8_t>>>& incomingMessages,
            std::vector<uint8_t>& completeMessage);

        // Create acknowledgment
        static std::vector<uint8_t> CreateAck(uint32_t messageId, uint32_t chunkIndex);

    private:
        static uint32_t GenerateMessageId();
    };

} // namespace BleServer

// C API for Unity integration
extern "C" {
    // Server lifecycle
    BLE_API void* BleServer_Create();
    BLE_API void BleServer_Destroy(void* server);
    BLE_API int BleServer_Initialize(void* server, const char* deviceName, bool requiresPairing);
    BLE_API void BleServer_Shutdown(void* server);
    BLE_API bool BleServer_IsInitialized(void* server);

    // Service management
    BLE_API int BleServer_AddService(void* server, const char* serviceUuid, bool isPrimary);
    BLE_API int BleServer_RemoveService(void* server, const char* serviceUuid);

    // Characteristic management
    BLE_API int BleServer_AddCharacteristic(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        uint32_t properties,
        uint32_t permissions);

    // Callbacks (using function pointers for Unity interop)
    typedef void (*ConnectionCallbackFn)(const char* deviceId, bool connected);
    typedef void (*DataCallbackFn)(const char* deviceId, const uint8_t* data, int dataLength);
    typedef void (*ErrorCallbackFn)(int error, const char* message);

    BLE_API void BleServer_SetConnectionCallback(void* server, ConnectionCallbackFn callback);
    BLE_API void BleServer_SetCharacteristicWriteCallback(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        DataCallbackFn callback);

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
}