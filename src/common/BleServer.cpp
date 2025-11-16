// BleServer.cpp - Common implementation and C API exports
#include "IBleServer.h"
#include <map>
#include <mutex>
#include <queue>
#include <cstring>

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
        ConnectionCallbackFn connectionCallback = nullptr;
        DataCallbackFn dataCallback = nullptr;
        ErrorCallbackFn errorCallback = nullptr;
        std::mutex mutex;
    };

    struct BleChunkerWrapper {
        std::map<uint32_t, std::vector<std::vector<uint8_t>>> incomingMessages;
        std::queue<std::vector<uint8_t>> completeMessages;
        std::mutex mutex;
    };

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

        auto wrapper = static_cast<BleServerWrapper*>(server);

        BleServer::ServerConfig config;
        config.deviceName = deviceName ? deviceName : "BLE Server";
        config.requiresPairing = requiresPairing;
        config.requiresBonding = false;
        config.maxConnections = 10;

        // Set up internal callbacks
        wrapper->server->SetConnectionCallback([wrapper](const std::string& deviceId, bool connected) {
            std::lock_guard<std::mutex> lock(wrapper->mutex);
            if (wrapper->connectionCallback) {
                wrapper->connectionCallback(deviceId.c_str(), connected);
            }
            });

        wrapper->server->SetErrorCallback([wrapper](BleServer::BleError error, const std::string& message) {
            std::lock_guard<std::mutex> lock(wrapper->mutex);
            if (wrapper->errorCallback) {
                wrapper->errorCallback(static_cast<int>(error), message.c_str());
            }
            });

        return static_cast<int>(wrapper->server->Initialize(config));
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

        BleServer::CharacteristicConfig config;
        config.uuid = characteristicUuid;
        config.properties = properties;
        config.permissions = permissions;
        config.maxLength = 512;

        return static_cast<int>(wrapper->server->AddCharacteristic(serviceUuid, config));
    }

    BLE_API void BleServer_SetConnectionCallback(void* server, ConnectionCallbackFn callback) {
        if (!server) return;

        auto wrapper = static_cast<BleServerWrapper*>(server);
        std::lock_guard<std::mutex> lock(wrapper->mutex);
        wrapper->connectionCallback = callback;
    }

    BLE_API void BleServer_SetCharacteristicWriteCallback(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        DataCallbackFn callback) {
        if (!server || !serviceUuid || !characteristicUuid) return;

        auto wrapper = static_cast<BleServerWrapper*>(server);

        // Create a lambda that converts to C callback
        auto writeCallback = [wrapper, callback](const std::string& deviceId, const std::vector<uint8_t>& data) {
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

} // extern "C"
