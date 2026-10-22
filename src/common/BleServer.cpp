// BleServer.cpp - Common implementation and C API exports
#include "IBleServer.h"
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <cstring>
#include <exception>
#include <algorithm>

namespace BleServer {

    // Platform-specific factory function (implemented in each platform)
    extern std::unique_ptr<IBleServer> CreatePlatformServer();

    // Factory method implementation
    std::unique_ptr<IBleServer> IBleServer::Create() {
        return CreatePlatformServer();
    }

} // namespace BleServer

// C API Implementation
extern "C" {

    struct BleServerWrapper {
        std::unique_ptr<BleServer::IBleServer> server;
        std::map<std::string, std::map<std::string, BleServer::CharacteristicWriteCallback>> writeCallbacks;
        std::map<std::string, std::map<std::string, BleServer::CharacteristicReadCallback>> readCallbacks;
        ConnectionCallbackFn connectionCallback = nullptr;
        ConnectionCallbackExFn connectionCallbackEx = nullptr;
        void* connectionUserData = nullptr;
        DataCallbackFn dataCallback = nullptr;
        ErrorCallbackFn errorCallback = nullptr;
        ErrorCallbackExFn errorCallbackEx = nullptr;
        void* errorUserData = nullptr;
        std::string lastError;
        std::mutex mutex;
    };

    struct BleChunkerWrapper {
        std::map<uint32_t, std::vector<std::vector<uint8_t>>> incomingMessages;
        std::queue<std::vector<uint8_t>> completeMessages;
        std::mutex mutex;
    };

    static constexpr int BLE_SERVER_API_VERSION = 2;

    static void SetLastError(BleServerWrapper* wrapper, const std::string& message) {
        if (!wrapper) {
            return;
        }
        std::lock_guard<std::mutex> lock(wrapper->mutex);
        wrapper->lastError = message;
    }

    static BleServer::ServerConfig ToServerConfig(const BleServerConfigC* cConfig) {
        BleServer::ServerConfig config;
        if (!cConfig) {
            return config;
        }

        config.deviceName = cConfig->deviceName ? cConfig->deviceName : "BLE Server";
        config.requiresPairing = cConfig->requiresPairing;
        config.requiresBonding = cConfig->requiresBonding;
        config.maxConnections = cConfig->maxConnections > 0 ? cConfig->maxConnections : 10;
        return config;
    }

    static BleServer::CharacteristicConfig ToCharacteristicConfig(const char* uuid,
        uint32_t properties,
        uint32_t permissions,
        const uint8_t* initialValue,
        int initialValueLength,
        int maxLength) {
        BleServer::CharacteristicConfig config;
        config.uuid = uuid ? uuid : "";
        config.properties = properties;
        config.permissions = permissions;
        if (initialValue && initialValueLength > 0) {
            config.initialValue.assign(initialValue, initialValue + initialValueLength);
        }
        config.maxLength = maxLength > 0 ? static_cast<size_t>(maxLength) : 512;
        return config;
    }

    BLE_API void* BleServer_Create() {
        try {
            auto wrapper = new BleServerWrapper();
            wrapper->server = BleServer::IBleServer::Create();

            // Check if server was created successfully
            if (!wrapper->server) {
                delete wrapper;
                return nullptr;
            }

            return wrapper;
        }
        catch (const std::exception& e) {
            // Log error if possible
            return nullptr;
        }
        catch (...) {
            return nullptr;
        }
    }

    BLE_API void BleServer_Destroy(void* server) {
        if (server) {
            delete static_cast<BleServerWrapper*>(server);
        }
    }

    BLE_API int BleServer_Initialize(void* server, const char* deviceName, bool requiresPairing) {
        if (!server) return static_cast<int>(BleServer::BleError::InvalidParameter);

        BleServerConfigC config{};
        config.size = sizeof(BleServerConfigC);
        config.deviceName = deviceName;
        config.requiresPairing = requiresPairing;
        config.requiresBonding = false;
        config.maxConnections = 10;

        return BleServer_InitializeEx(server, &config);
    }

    BLE_API int BleServer_InitializeEx(void* server, const BleServerConfigC* config) {
        if (!server || !config) return static_cast<int>(BleServer::BleError::InvalidParameter);

        auto wrapper = static_cast<BleServerWrapper*>(server);

        BleServer::ServerConfig nativeConfig = ToServerConfig(config);

        // Set up internal callbacks
        wrapper->server->SetConnectionCallback([wrapper](const std::string& deviceId, bool connected) {
            std::lock_guard<std::mutex> lock(wrapper->mutex);
            if (wrapper->connectionCallback) {
                wrapper->connectionCallback(deviceId.c_str(), connected);
            }
            if (wrapper->connectionCallbackEx) {
                wrapper->connectionCallbackEx(deviceId.c_str(), connected, wrapper->connectionUserData);
            }
            });

        wrapper->server->SetErrorCallback([wrapper](BleServer::BleError error, const std::string& message) {
            std::lock_guard<std::mutex> lock(wrapper->mutex);
            wrapper->lastError = message;
            if (wrapper->errorCallback) {
                wrapper->errorCallback(static_cast<int>(error), message.c_str());
            }
            if (wrapper->errorCallbackEx) {
                wrapper->errorCallbackEx(static_cast<int>(error), message.c_str(), wrapper->errorUserData);
            }
            });

        auto result = wrapper->server->Initialize(nativeConfig);
        if (result != BleServer::BleError::Success) {
            SetLastError(wrapper, "Initialize failed with error code " + std::to_string(static_cast<int>(result)));
        }
        return static_cast<int>(result);
    }

    BLE_API void BleServer_Shutdown(void* server) {
        if (!server) return;

        auto wrapper = static_cast<BleServerWrapper*>(server);
        wrapper->server->Shutdown();
    }

    BLE_API bool BleServer_IsInitialized(void* server) {
        if (!server) return false;

        auto wrapper = static_cast<BleServerWrapper*>(server);
        return wrapper->server->IsInitialized();
    }

    BLE_API int BleServer_GetApiVersion() {
        return BLE_SERVER_API_VERSION;
    }

    BLE_API const char* BleServer_GetBuildInfo() {
#if defined(_WIN32)
        return "BluetoothServer native plugin v1.0.0 api=2 platform=windows";
#elif defined(__APPLE__)
        return "BluetoothServer native plugin v1.0.0 api=2 platform=macos";
#elif defined(__linux__)
        return "BluetoothServer native plugin v1.0.0 api=2 platform=linux";
#else
        return "BluetoothServer native plugin v1.0.0 api=2 platform=unknown";
#endif
    }

    BLE_API const char* BleServer_GetLastError(void* server) {
        if (!server) return "Invalid server";

        auto wrapper = static_cast<BleServerWrapper*>(server);
        std::lock_guard<std::mutex> lock(wrapper->mutex);
        return wrapper->lastError.c_str();
    }

    BLE_API uint32_t BleServer_GetCapabilities(void* server) {
        if (!server) return 0;

        auto wrapper = static_cast<BleServerWrapper*>(server);
        return wrapper->server->GetCapabilities();
    }

    BLE_API int BleServer_GetStatus(void* server, char* buffer, int* bufferLength) {
        if (!server || !bufferLength) {
            return static_cast<int>(BleServer::BleError::InvalidParameter);
        }

        auto wrapper = static_cast<BleServerWrapper*>(server);
        std::string status = wrapper->server ? wrapper->server->DescribeStatus() : std::string("server not created");

        const int requiredLength = static_cast<int>(status.size() + 1);
        if (!buffer || *bufferLength < requiredLength) {
            *bufferLength = requiredLength;
            return static_cast<int>(BleServer::BleError::InvalidParameter);
        }

        std::memcpy(buffer, status.c_str(), static_cast<size_t>(requiredLength));
        *bufferLength = requiredLength;
        return static_cast<int>(BleServer::BleError::Success);
    }

    BLE_API void BleServer_LogStatus(void* server);

    // Diagnostics (runtime description + simple option getters/setters)
    BLE_API std::string BleServer_GetDescription(void* server);
    BLE_API std::optional<int> BleServer_GetOptionInt(const char* key);
    BLE_API void BleServer_SetOptionInt(const char* key, int value);

    BLE_API int BleServer_AddService(void* server, const char* serviceUuid, bool isPrimary) {
        if (!server || !serviceUuid) return static_cast<int>(BleServer::BleError::InvalidParameter);

        auto wrapper = static_cast<BleServerWrapper*>(server);

        BleServer::ServiceConfig config;
        config.uuid = serviceUuid;
        config.isPrimary = isPrimary;

        return static_cast<int>(wrapper->server->AddService(config));
    }

    BLE_API int BleServer_RemoveService(void* server, const char* serviceUuid) {
        if (!server || !serviceUuid) return static_cast<int>(BleServer::BleError::InvalidParameter);

        auto wrapper = static_cast<BleServerWrapper*>(server);
        return static_cast<int>(wrapper->server->RemoveService(serviceUuid));
    }

    BLE_API int BleServer_AddCharacteristic(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        uint32_t properties,
        uint32_t permissions) {
        if (!server || !serviceUuid || !characteristicUuid) {
            return static_cast<int>(BleServer::BleError::InvalidParameter);
        }

        auto wrapper = static_cast<BleServerWrapper*>(server);

        auto config = ToCharacteristicConfig(characteristicUuid, properties, permissions, nullptr, 0, 512);

        return static_cast<int>(wrapper->server->AddCharacteristic(serviceUuid, config));
    }

    BLE_API int BleServer_AddCharacteristicEx(void* server,
        const char* serviceUuid,
        const BleCharacteristicConfigC* config) {
        if (!server || !serviceUuid || !config || !config->uuid) {
            return static_cast<int>(BleServer::BleError::InvalidParameter);
        }

        auto wrapper = static_cast<BleServerWrapper*>(server);
        auto nativeConfig = ToCharacteristicConfig(
            config->uuid,
            config->properties,
            config->permissions,
            config->initialValue,
            config->initialValueLength,
            config->maxLength);

        return static_cast<int>(wrapper->server->AddCharacteristic(serviceUuid, nativeConfig));
    }

    BLE_API int BleServer_RemoveCharacteristic(void* server,
        const char* serviceUuid,
        const char* characteristicUuid) {
        if (!server || !serviceUuid || !characteristicUuid) {
            return static_cast<int>(BleServer::BleError::InvalidParameter);
        }

        auto wrapper = static_cast<BleServerWrapper*>(server);
        return static_cast<int>(wrapper->server->RemoveCharacteristic(serviceUuid, characteristicUuid));
    }

    BLE_API int BleServer_AddDescriptor(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        const BleDescriptorConfigC* config) {
        if (!server || !serviceUuid || !characteristicUuid || !config || !config->uuid) {
            return static_cast<int>(BleServer::BleError::InvalidParameter);
        }

        BleServer::DescriptorConfig nativeConfig;
        nativeConfig.uuid = config->uuid;
        nativeConfig.permissions = config->permissions;
        if (config->initialValue && config->initialValueLength > 0) {
            nativeConfig.initialValue.assign(config->initialValue, config->initialValue + config->initialValueLength);
        }

        auto wrapper = static_cast<BleServerWrapper*>(server);
        return static_cast<int>(wrapper->server->AddDescriptor(serviceUuid, characteristicUuid, nativeConfig));
    }

    BLE_API void BleServer_SetConnectionCallback(void* server, ConnectionCallbackFn callback) {
        if (!server) return;

        auto wrapper = static_cast<BleServerWrapper*>(server);
        std::lock_guard<std::mutex> lock(wrapper->mutex);
        wrapper->connectionCallback = callback;
    }

    BLE_API void BleServer_SetConnectionCallbackEx(void* server, ConnectionCallbackExFn callback, void* userData) {
        if (!server) return;

        auto wrapper = static_cast<BleServerWrapper*>(server);
        std::lock_guard<std::mutex> lock(wrapper->mutex);
        wrapper->connectionCallbackEx = callback;
        wrapper->connectionUserData = userData;
    }

    BLE_API void BleServer_SetErrorCallback(void* server, ErrorCallbackFn callback) {
        if (!server) return;

        auto wrapper = static_cast<BleServerWrapper*>(server);
        std::lock_guard<std::mutex> lock(wrapper->mutex);
        wrapper->errorCallback = callback;
    }

    BLE_API void BleServer_SetErrorCallbackEx(void* server, ErrorCallbackExFn callback, void* userData) {
        if (!server) return;

        auto wrapper = static_cast<BleServerWrapper*>(server);
        std::lock_guard<std::mutex> lock(wrapper->mutex);
        wrapper->errorCallbackEx = callback;
        wrapper->errorUserData = userData;
    }

    BLE_API int BleServer_SetCharacteristicReadCallback(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        ReadCallbackFn callback,
        void* userData) {
        if (!server || !serviceUuid || !characteristicUuid) {
            return static_cast<int>(BleServer::BleError::InvalidParameter);
        }

        auto wrapper = static_cast<BleServerWrapper*>(server);
        auto readCallback = [callback, userData](const std::string& deviceId) {
            std::vector<uint8_t> data;
            if (!callback) {
                return data;
            }

            int requiredLength = callback(deviceId.c_str(), nullptr, 0, userData);
            if (requiredLength <= 0) {
                return data;
            }

            data.resize(static_cast<size_t>(requiredLength));
            int written = callback(deviceId.c_str(), data.data(), requiredLength, userData);
            if (written < 0) {
                data.clear();
                return data;
            }

            if (written < static_cast<int>(data.size())) {
                data.resize(static_cast<size_t>(written));
            }
            return data;
            };

        {
            std::lock_guard<std::mutex> lock(wrapper->mutex);
            wrapper->readCallbacks[serviceUuid][characteristicUuid] = readCallback;
        }

        return static_cast<int>(wrapper->server->SetCharacteristicReadCallback(serviceUuid, characteristicUuid, readCallback));
    }

    BLE_API void BleServer_SetCharacteristicWriteCallback(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        DataCallbackFn callback) {
        if (!server || !serviceUuid || !characteristicUuid) return;

        auto wrapper = static_cast<BleServerWrapper*>(server);

        // Create a lambda that converts to C callback
        auto writeCallback = [callback](const std::string& deviceId, const std::vector<uint8_t>& data) {
            if (callback) {
                callback(deviceId.c_str(), data.data(), static_cast<int>(data.size()));
            }
            };

        {
            std::lock_guard<std::mutex> lock(wrapper->mutex);
            wrapper->writeCallbacks[serviceUuid][characteristicUuid] = writeCallback;
        }

        wrapper->server->SetCharacteristicWriteCallback(serviceUuid, characteristicUuid, writeCallback);
    }

    BLE_API int BleServer_SetCharacteristicWriteCallbackEx(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        DataCallbackExFn callback,
        void* userData) {
        if (!server || !serviceUuid || !characteristicUuid) {
            return static_cast<int>(BleServer::BleError::InvalidParameter);
        }

        auto wrapper = static_cast<BleServerWrapper*>(server);

        auto writeCallback = [callback, userData](const std::string& deviceId, const std::vector<uint8_t>& data) {
            if (callback) {
                callback(deviceId.c_str(), data.data(), static_cast<int>(data.size()), userData);
            }
            };

        {
            std::lock_guard<std::mutex> lock(wrapper->mutex);
            wrapper->writeCallbacks[serviceUuid][characteristicUuid] = writeCallback;
        }

        return static_cast<int>(wrapper->server->SetCharacteristicWriteCallback(serviceUuid, characteristicUuid, writeCallback));
    }

    BLE_API int BleServer_StartAdvertising(void* server) {
        if (!server) return static_cast<int>(BleServer::BleError::InvalidParameter);

        auto wrapper = static_cast<BleServerWrapper*>(server);
        return static_cast<int>(wrapper->server->StartAdvertising());
    }

    BLE_API int BleServer_StopAdvertising(void* server) {
        if (!server) return static_cast<int>(BleServer::BleError::InvalidParameter);

        auto wrapper = static_cast<BleServerWrapper*>(server);
        return static_cast<int>(wrapper->server->StopAdvertising());
    }

    BLE_API bool BleServer_IsAdvertising(void* server) {
        if (!server) return false;

        auto wrapper = static_cast<BleServerWrapper*>(server);
        return wrapper->server->IsAdvertising();
    }

    BLE_API int BleServer_UpdateCharacteristicValue(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        const uint8_t* value,
        int valueLength) {
        if (!server || !serviceUuid || !characteristicUuid || !value || valueLength < 0) {
            return static_cast<int>(BleServer::BleError::InvalidParameter);
        }

        auto wrapper = static_cast<BleServerWrapper*>(server);
        std::vector<uint8_t> data(value, value + valueLength);

        return static_cast<int>(wrapper->server->UpdateCharacteristicValue(
            serviceUuid, characteristicUuid, data));
    }

    BLE_API int BleServer_NotifyCharacteristicChanged(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        const char* deviceId) {
        if (!server || !serviceUuid || !characteristicUuid) {
            return static_cast<int>(BleServer::BleError::InvalidParameter);
        }

        auto wrapper = static_cast<BleServerWrapper*>(server);
        std::string device = deviceId ? deviceId : "";

        return static_cast<int>(wrapper->server->NotifyCharacteristicChanged(
            serviceUuid, characteristicUuid, device));
    }

    BLE_API int BleServer_GetConnectedDeviceCount(void* server) {
        if (!server) return -1;

        auto wrapper = static_cast<BleServerWrapper*>(server);
        auto devices = wrapper->server->GetConnectedDevices();
        return static_cast<int>(devices.size());
    }

    BLE_API int BleServer_GetConnectedDeviceId(void* server, int index, char* buffer, int* bufferLength) {
        if (!server || index < 0 || !bufferLength) {
            return static_cast<int>(BleServer::BleError::InvalidParameter);
        }

        auto wrapper = static_cast<BleServerWrapper*>(server);
        auto devices = wrapper->server->GetConnectedDevices();
        if (index >= static_cast<int>(devices.size())) {
            return static_cast<int>(BleServer::BleError::InvalidParameter);
        }

        const auto& deviceId = devices[static_cast<size_t>(index)];
        const int requiredLength = static_cast<int>(deviceId.size() + 1);
        if (!buffer || *bufferLength < requiredLength) {
            *bufferLength = requiredLength;
            return static_cast<int>(BleServer::BleError::InvalidParameter);
        }

        std::memcpy(buffer, deviceId.c_str(), static_cast<size_t>(requiredLength));
        *bufferLength = requiredLength;
        return static_cast<int>(BleServer::BleError::Success);
    }

    BLE_API int BleServer_DisconnectDevice(void* server, const char* deviceId) {
        if (!server || !deviceId) {
            return static_cast<int>(BleServer::BleError::InvalidParameter);
        }

        auto wrapper = static_cast<BleServerWrapper*>(server);
        return static_cast<int>(wrapper->server->DisconnectDevice(deviceId));
    }

    // Chunker C API
    BLE_API void* BleChunker_Create() {
        return new BleChunkerWrapper();
    }

    BLE_API void BleChunker_Destroy(void* chunker) {
        if (chunker) {
            delete static_cast<BleChunkerWrapper*>(chunker);
        }
    }

    BLE_API int BleChunker_SendMessage(void* chunker, void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        const uint8_t* data,
        int dataLength) {
        if (!chunker || !server || !serviceUuid || !characteristicUuid || !data || dataLength < 0) {
            return static_cast<int>(BleServer::BleError::InvalidParameter);
        }

        auto wrapper = static_cast<BleServerWrapper*>(server);
        std::vector<uint8_t> message(data, data + dataLength);

        auto chunks = BleServer::BleMessageChunker::CreateChunks(message);

        for (const auto& chunk : chunks) {
            auto result = wrapper->server->UpdateCharacteristicValue(
                serviceUuid, characteristicUuid, chunk);

            if (result != BleServer::BleError::Success) {
                return static_cast<int>(result);
            }

            // Notify all connected devices
            wrapper->server->NotifyCharacteristicChanged(serviceUuid, characteristicUuid, "");
        }

        return static_cast<int>(BleServer::BleError::Success);
    }

    BLE_API bool BleChunker_ProcessReceivedData(void* chunker,
        const uint8_t* data,
        int dataLength,
        uint8_t* completeMessage,
        int* completeMessageLength) {
        if (!chunker || !data || dataLength < 0) return false;

        auto wrapper = static_cast<BleChunkerWrapper*>(chunker);
        std::lock_guard<std::mutex> lock(wrapper->mutex);

        std::vector<uint8_t> chunk(data, data + dataLength);
        std::vector<uint8_t> complete;

        if (BleServer::BleMessageChunker::ProcessChunk(chunk, wrapper->incomingMessages, complete)) {
            wrapper->completeMessages.push(complete);
        }

        // Check if we have any complete messages
        if (!wrapper->completeMessages.empty()) {
            auto& msg = wrapper->completeMessages.front();

            if (completeMessage && completeMessageLength && *completeMessageLength >= static_cast<int>(msg.size())) {
                std::memcpy(completeMessage, msg.data(), msg.size());
                *completeMessageLength = static_cast<int>(msg.size());
                wrapper->completeMessages.pop();
                return true;
            }
        }

        return false;
    }

    BLE_API int BleChunker_ProcessIncoming(void* chunker,
        const uint8_t* data,
        int dataLength,
        uint8_t* ackBuffer,
        int* ackLength) {
        if (!chunker || !data || dataLength < 0) {
            return static_cast<int>(BleServer::BleError::InvalidParameter);
        }

        auto wrapper = static_cast<BleChunkerWrapper*>(chunker);
        std::lock_guard<std::mutex> lock(wrapper->mutex);

        std::vector<uint8_t> packet(data, data + dataLength);
        if (!BleServer::BleMessageChunker::IsValidChunk(packet)) {
            return static_cast<int>(BleServer::BleError::InvalidParameter);
        }

        const int ackCapacity = ackLength ? *ackLength : 0;

        if (BleServer::BleMessageChunker::IsAck(packet)) {
            return static_cast<int>(BleServer::BleError::Success);
        }

        auto ack = BleServer::BleMessageChunker::CreateAck(
            BleServer::BleMessageChunker::GetMessageId(packet),
            BleServer::BleMessageChunker::GetChunkIndex(packet));

        if (ackLength) {
            const int requiredLength = static_cast<int>(ack.size());
            if (ackBuffer && ackCapacity >= requiredLength) {
                std::memcpy(ackBuffer, ack.data(), ack.size());
            }
            *ackLength = requiredLength;
        }

        std::vector<uint8_t> complete;
        if (BleServer::BleMessageChunker::ProcessChunk(packet, wrapper->incomingMessages, complete)) {
            wrapper->completeMessages.push(complete);
            return 1;
        }

        return 0;
    }

    BLE_API int BleChunker_GetNextMessageSize(void* chunker) {
        if (!chunker) return -1;

        auto wrapper = static_cast<BleChunkerWrapper*>(chunker);
        std::lock_guard<std::mutex> lock(wrapper->mutex);
        if (wrapper->completeMessages.empty()) {
            return 0;
        }
        return static_cast<int>(wrapper->completeMessages.front().size());
    }

    BLE_API bool BleChunker_ReadNextMessage(void* chunker,
        uint8_t* completeMessage,
        int* completeMessageLength) {
        if (!chunker || !completeMessageLength) return false;

        auto wrapper = static_cast<BleChunkerWrapper*>(chunker);
        std::lock_guard<std::mutex> lock(wrapper->mutex);
        if (wrapper->completeMessages.empty()) {
            *completeMessageLength = 0;
            return false;
        }

        auto& msg = wrapper->completeMessages.front();
        const int requiredLength = static_cast<int>(msg.size());
        if (!completeMessage || *completeMessageLength < requiredLength) {
            *completeMessageLength = requiredLength;
            return false;
        }

        if (!msg.empty()) {
            std::memcpy(completeMessage, msg.data(), msg.size());
        }
        *completeMessageLength = requiredLength;
        wrapper->completeMessages.pop();
        return true;
    }

    BLE_API int BleChunker_CleanupTimedOut(void* chunker, int timeoutMs) {
        (void)timeoutMs;
        if (!chunker) return static_cast<int>(BleServer::BleError::InvalidParameter);
        return 0;
    }

    // --- Diagnostics stubs (T2.3) ---

    BLE_API void BleServer_LogStatus(void* server) {
        if (!server) {
            std::fprintf(stderr, "[BluetoothServer] LogStatus called with null server\n");
            return;
        }
        auto wrapper = static_cast<BleServerWrapper*>(server);
        std::string status = wrapper->server ? wrapper->server->DescribeStatus() : std::string("server not created");
        std::fprintf(stderr, "[BluetoothServer] %s\n", status.c_str());
    }

    BLE_API std::string BleServer_GetDescription(void* server) {
        if (!server) return std::string();
        auto wrapper = static_cast<BleServerWrapper*>(server);
        return wrapper->server ? wrapper->server->DescribeStatus() : std::string("server not created");
    }

    BLE_API std::optional<int> BleServer_GetOptionInt(const char* key) {
        (void)key;
        return std::nullopt;
    }

    BLE_API void BleServer_SetOptionInt(const char* key, int value) {
        (void)key;
        (void)value;
    }

} // extern "C"
