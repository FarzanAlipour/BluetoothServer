// BleServer.cpp - Common implementation and C API exports.
//
// Design rules (see DEVELOPER_GUIDE.md):
//  * Handles are validated against a registry: null, unknown, destroyed handles are rejected with
//    InvalidParameter instead of being dereferenced. Each call holds a shared_ptr, so a concurrent
//    Destroy cannot free the wrapper under a running call.
//  * User callbacks are never invoked while an internal lock is held: the callback is copied under
//    the lock, the lock is released, then it is invoked.
//  * Callbacks are gated by CallbackState::active, cleared by Shutdown/Destroy before the backend
//    is stopped, so nothing new is delivered after Shutdown or Destroy returns.
//  * No exception crosses the C boundary: every entry point is wrapped in a guard.
#include "IBleServer.h"
#include "BleUtilities.h"
#include "BleServerInternal.h"
#include "BleServerLinkHooks.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <queue>

#ifndef BLE_BUILD_PLATFORM
#define BLE_BUILD_PLATFORM "unknown"
#endif

namespace BleServer {

    // Platform-specific factory function (implemented in each platform)
    extern std::unique_ptr<IBleServer> CreatePlatformServer();

    // Factory method implementation
    std::unique_ptr<IBleServer> IBleServer::Create() {
        return CreatePlatformServer();
    }

} // namespace BleServer

namespace {

    using BleServer::BleError;
    using BleServer::IBleServer;
    using BleServer::BleMessageChunker;
    namespace Internal = BleServer::Internal;

    constexpr int Err(BleError e) { return static_cast<int>(e); }
    constexpr int kApiVersion = 3;  // keep in sync with "api=" in BleServer_GetBuildInfo

    // ---- guards: no exception escapes an entry point ----
    template <class F> int GuardInt(F&& f) {
        try { return f(); } catch (...) { return Err(BleError::Unknown); }
    }
    template <class F> void GuardVoid(F&& f) {
        try { f(); } catch (...) {}
    }
    template <class R, class F> R GuardRet(R fallback, F&& f) {
        try { return f(); } catch (...) { return fallback; }
    }

    // Callbacks currently running on this thread (any server). A callback that re-enters
    // Shutdown/Destroy must not wait for itself to finish.
    thread_local int t_callbackDepth = 0;

    // ---- state shared between the wrapper and backend callbacks ----
    struct CallbackState {
        std::mutex mutex;
        std::condition_variable idle;
        bool active = false;  // true between a successful Initialize and Shutdown/Destroy
        int inFlight = 0;     // user callbacks currently executing (lock NOT held while they run)
        ConnectionCallbackFn connectionCallback = nullptr;
        ConnectionCallbackExFn connectionCallbackEx = nullptr;
        void* connectionUserData = nullptr;
        ErrorCallbackFn errorCallback = nullptr;
        ErrorCallbackExFn errorCallbackEx = nullptr;
        void* errorUserData = nullptr;
        std::string lastError;
        std::shared_ptr<Internal::LinkHooks> link;  // attached BleLink (BleLinkAbi.cpp), at most one

        void SetActive(bool value) {
            std::lock_guard<std::mutex> lock(mutex);
            active = value;
        }
        bool IsActive() {
            std::lock_guard<std::mutex> lock(mutex);
            return active;
        }
        void SetLastError(const std::string& message) {
            std::lock_guard<std::mutex> lock(mutex);
            lastError = message;
        }
        // Stop new deliveries, then wait until every running callback has returned. After this
        // returns (on a non-callback thread) no user callback is running or will start, so the
        // caller may free what the callbacks use (e.g. Unity GCHandles).
        void DeactivateAndDrain() {
            std::unique_lock<std::mutex> lock(mutex);
            active = false;
            if (t_callbackDepth > 0) return;  // called from inside a callback: cannot wait for ourselves
            idle.wait(lock, [this] { return inFlight == 0; });
        }
        // Returns false (and does not count) if callbacks are not active.
        bool TryEnter() {
            std::lock_guard<std::mutex> lock(mutex);
            if (!active) return false;
            ++inFlight;
            return true;
        }
        void Leave() {
            std::lock_guard<std::mutex> lock(mutex);
            if (--inFlight == 0) idle.notify_all();
        }
    };

    // RAII: one running user callback.
    class CallbackScope {
    public:
        explicit CallbackScope(CallbackState& st) : st_(st), entered_(st.TryEnter()) {
            if (entered_) ++t_callbackDepth;
        }
        ~CallbackScope() {
            if (entered_) { --t_callbackDepth; st_.Leave(); }
        }
        bool entered() const { return entered_; }
        CallbackScope(const CallbackScope&) = delete;
        CallbackScope& operator=(const CallbackScope&) = delete;
    private:
        CallbackState& st_;
        bool entered_;
    };

    void DeliverConnection(const std::shared_ptr<CallbackState>& st, const std::string& deviceId, bool connected) {
        CallbackScope scope(*st);
        if (!scope.entered()) return;
        ConnectionCallbackFn cb;
        ConnectionCallbackExFn cbEx;
        void* user;
        {
            std::lock_guard<std::mutex> lock(st->mutex);
            cb = st->connectionCallback;
            cbEx = st->connectionCallbackEx;
            user = st->connectionUserData;
        }  // lock released before any user code runs
        if (cb) cb(deviceId.c_str(), connected);
        if (cbEx) cbEx(deviceId.c_str(), connected, user);
    }

    void DeliverError(const std::shared_ptr<CallbackState>& st, BleError error, const std::string& message) {
        st->SetLastError(message);
        CallbackScope scope(*st);
        if (!scope.entered()) return;
        ErrorCallbackFn cb;
        ErrorCallbackExFn cbEx;
        void* user;
        {
            std::lock_guard<std::mutex> lock(st->mutex);
            cb = st->errorCallback;
            cbEx = st->errorCallbackEx;
            user = st->errorUserData;
        }
        if (cb) cb(static_cast<int>(error), message.c_str());
        if (cbEx) cbEx(static_cast<int>(error), message.c_str(), user);
    }

    // Backend event -> attached BleLink. Gated like a user callback (nothing after Shutdown/Destroy).
    template <class F> void ForwardToLink(const std::shared_ptr<CallbackState>& st, F&& f) {
        CallbackScope scope(*st);
        if (!scope.entered()) return;
        std::shared_ptr<Internal::LinkHooks> link;
        {
            std::lock_guard<std::mutex> lock(st->mutex);
            link = st->link;
        }
        if (link) f(*link);
    }

    // Detaches the link and tells it the server goes away. Caller holds lifecycleMutex and has
    // deactivated callbacks; the backend is still up so the link can send CANCEL.
    void ShutdownLink(CallbackState& st) {
        std::shared_ptr<Internal::LinkHooks> link;
        {
            std::lock_guard<std::mutex> lock(st.mutex);
            link.swap(st.link);
        }
        if (link) link->OnServerShutdown();
    }

    struct BleServerWrapper {
        std::shared_ptr<CallbackState> state = std::make_shared<CallbackState>();
        std::unique_ptr<IBleServer> server;
        std::mutex lifecycleMutex;  // serialises Initialize / Shutdown / Destroy
    };

    // Sent message waiting for ACKs (retransmission contract: see BleChunker_CleanupTimedOut).
    struct OutstandingMessage {
        uint32_t messageId = 0;
        std::weak_ptr<BleServerWrapper> server;
        std::string serviceUuid;
        std::string characteristicUuid;
        std::vector<std::vector<uint8_t>> chunks;
        std::vector<bool> acked;
        std::vector<int> retries;
        std::chrono::steady_clock::time_point lastSent;
    };

    struct BleChunkerWrapper {
        std::map<uint32_t, std::vector<std::vector<uint8_t>>> incomingMessages;
        std::deque<uint32_t> incomingOrder;      // arrival order, for bounded eviction
        std::deque<uint32_t> recentlyCompleted;  // ids of finished messages (duplicate suppression)
        std::queue<std::vector<uint8_t>> completeMessages;
        std::deque<OutstandingMessage> outstanding;
        std::mutex mutex;
    };

    // Handle registry: the only way a void* becomes a pointer.
    template <class T> class Registry {
    public:
        void* Add(const std::shared_ptr<T>& item) {
            std::lock_guard<std::mutex> lock(m_);
            items_[item.get()] = item;
            return item.get();
        }
        std::shared_ptr<T> Find(const void* handle) {
            if (!handle) return nullptr;
            std::lock_guard<std::mutex> lock(m_);
            auto it = items_.find(handle);
            return it == items_.end() ? nullptr : it->second;
        }
        std::shared_ptr<T> Remove(const void* handle) {
            if (!handle) return nullptr;
            std::lock_guard<std::mutex> lock(m_);
            auto it = items_.find(handle);
            if (it == items_.end()) return nullptr;
            auto item = it->second;
            items_.erase(it);
            return item;
        }
        size_t Size() {
            std::lock_guard<std::mutex> lock(m_);
            return items_.size();
        }
    private:
        std::mutex m_;
        std::map<const void*, std::shared_ptr<T>> items_;
    };

    // Intentionally leaked: avoids static-destruction-order problems with late callers.
    Registry<BleServerWrapper>& Servers() { static auto* r = new Registry<BleServerWrapper>(); return *r; }
    Registry<BleChunkerWrapper>& Chunkers() { static auto* r = new Registry<BleChunkerWrapper>(); return *r; }

    BleServer::ServerConfig ToServerConfig(const BleServerConfigC* cConfig) {
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

    BleServer::CharacteristicConfig ToCharacteristicConfig(const char* uuid,
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

    bool ValidUuid(const char* uuid) {
        return uuid && BleServer::Utils::IsValidUuid(uuid);
    }

    // Copy `text` (plus NUL) into buffer following the "query required length" convention.
    int CopyOut(const std::string& text, char* buffer, int* bufferLength) {
        const int requiredLength = static_cast<int>(text.size() + 1);
        if (!buffer || *bufferLength < requiredLength) {
            *bufferLength = requiredLength;
            return Err(BleError::InvalidParameter);
        }
        std::memcpy(buffer, text.c_str(), static_cast<size_t>(requiredLength));
        *bufferLength = requiredLength;
        return Err(BleError::Success);
    }

    // Queue a finished message; the queue is bounded, the oldest unread message is dropped.
    // Caller holds wrapper.mutex.
    void PushComplete(BleChunkerWrapper& w, std::vector<uint8_t>&& message) {
        w.completeMessages.push(std::move(message));
        while (w.completeMessages.size() > Internal::kMaxCompleteMessages) w.completeMessages.pop();
    }

    // Receiving side. Returns true if `packet` completed a message (moved into `complete`).
    // Caller holds wrapper.mutex.
    bool IngestPacket(BleChunkerWrapper& w, const std::vector<uint8_t>& packet, std::vector<uint8_t>& complete) {
        const uint32_t id = BleMessageChunker::GetMessageId(packet);
        if (std::find(w.recentlyCompleted.begin(), w.recentlyCompleted.end(), id) != w.recentlyCompleted.end()) {
            return false;  // duplicate (retransmission) of an already delivered message
        }
        const bool isNew = w.incomingMessages.find(id) == w.incomingMessages.end();
        if (!BleMessageChunker::ProcessChunk(packet, w.incomingMessages, complete)) {
            if (isNew && w.incomingMessages.find(id) != w.incomingMessages.end()) {
                w.incomingOrder.push_back(id);
                while (w.incomingMessages.size() > Internal::kMaxPartialMessages && !w.incomingOrder.empty()) {
                    w.incomingMessages.erase(w.incomingOrder.front());  // drop the oldest partial message
                    w.incomingOrder.pop_front();
                }
            }
            return false;
        }
        w.incomingOrder.erase(std::remove(w.incomingOrder.begin(), w.incomingOrder.end(), id), w.incomingOrder.end());
        w.recentlyCompleted.push_back(id);
        while (w.recentlyCompleted.size() > 16) w.recentlyCompleted.pop_front();
        return true;
    }

} // namespace

namespace BleServer {
namespace Internal {

    size_t ChunkerBufferedChunks(void* chunker) {
        auto w = Chunkers().Find(chunker);
        if (!w) return 0;
        std::lock_guard<std::mutex> lock(w->mutex);
        size_t n = 0;
        for (const auto& m : w->incomingMessages) n += m.second.size();
        return n;
    }
    size_t ChunkerOutstandingMessages(void* chunker) {
        auto w = Chunkers().Find(chunker);
        if (!w) return 0;
        std::lock_guard<std::mutex> lock(w->mutex);
        return w->outstanding.size();
    }
    size_t ChunkerQueuedMessages(void* chunker) {
        auto w = Chunkers().Find(chunker);
        if (!w) return 0;
        std::lock_guard<std::mutex> lock(w->mutex);
        return w->completeMessages.size();
    }
    size_t LiveServerHandles() { return Servers().Size(); }
    size_t LiveChunkerHandles() { return Chunkers().Size(); }

    bool AcquireServer(void* serverHandle, ServerAccess& out) {
        auto wrapper = Servers().Find(serverHandle);
        if (!wrapper) return false;
        out.server = wrapper->server.get();
        out.keepAlive = wrapper;
        return true;
    }

    bool AttachLink(void* serverHandle, const std::shared_ptr<LinkHooks>& hooks) {
        auto wrapper = Servers().Find(serverHandle);
        if (!wrapper || !hooks) return false;
        std::lock_guard<std::mutex> lifecycle(wrapper->lifecycleMutex);  // no concurrent Shutdown
        if (!wrapper->server->IsInitialized()) return false;
        std::lock_guard<std::mutex> lock(wrapper->state->mutex);
        if (!wrapper->state->active || wrapper->state->link) return false;
        wrapper->state->link = hooks;
        return true;
    }

    void DetachLink(void* serverHandle, const LinkHooks* hooks) {
        auto wrapper = Servers().Find(serverHandle);
        if (!wrapper) return;
        std::lock_guard<std::mutex> lock(wrapper->state->mutex);
        if (wrapper->state->link.get() == hooks) wrapper->state->link.reset();
    }

    void SetServerLastError(void* serverHandle, const std::string& message) {
        if (auto wrapper = Servers().Find(serverHandle)) wrapper->state->SetLastError(message);
    }

} // namespace Internal
} // namespace BleServer

// C API Implementation
extern "C" {

    BLE_API void* BleServer_Create() {
        try {
            auto wrapper = std::make_shared<BleServerWrapper>();
            wrapper->server = IBleServer::Create();
            if (!wrapper->server) {
                return nullptr;
            }
            return Servers().Add(wrapper);
        }
        catch (...) {
            return nullptr;
        }
    }

    BLE_API void BleServer_Destroy(void* server) {
        GuardVoid([&] {
            auto wrapper = Servers().Remove(server);  // second Destroy / stale handle: no-op
            if (!wrapper) return;
            {
                std::lock_guard<std::mutex> lock(wrapper->lifecycleMutex);
                wrapper->state->DeactivateAndDrain();  // no new callbacks; running ones have returned
                ShutdownLink(*wrapper->state);         // BleLink: fail pending messages, send CANCEL
                wrapper->server->Shutdown();           // joins backend threads
            }
            // The wrapper is freed when the last in-flight call releases its reference.
        });
    }

    BLE_API int BleServer_Initialize(void* server, const char* deviceName, bool requiresPairing) {
        BleServerConfigC config{};
        config.size = sizeof(BleServerConfigC);
        config.deviceName = deviceName;
        config.requiresPairing = requiresPairing;
        config.requiresBonding = false;
        config.maxConnections = 10;
        return BleServer_InitializeEx(server, &config);
    }

    BLE_API int BleServer_InitializeEx(void* server, const BleServerConfigC* config) {
        return GuardInt([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper || !config) return Err(BleError::InvalidParameter);

            std::lock_guard<std::mutex> lifecycle(wrapper->lifecycleMutex);
            BleServer::ServerConfig nativeConfig = ToServerConfig(config);

            // Backend callbacks capture only the shared state, never the wrapper.
            std::shared_ptr<CallbackState> st = wrapper->state;
            wrapper->server->SetConnectionCallback([st](const std::string& deviceId, bool connected) {
                GuardVoid([&] {
                    ForwardToLink(st, [&](Internal::LinkHooks& l) { l.OnConnection(deviceId, connected); });
                    DeliverConnection(st, deviceId, connected);
                });
            });
            wrapper->server->SetErrorCallback([st](BleError error, const std::string& message) {
                GuardVoid([&] { DeliverError(st, error, message); });
            });
            // Per-central transport events only feed the BleLink engine (no C callbacks for them).
            wrapper->server->SetSubscriptionCallback([st](const std::string& svc, const std::string& chr,
                const std::string& deviceId, bool subscribed) {
                GuardVoid([&] {
                    ForwardToLink(st, [&](Internal::LinkHooks& l) { l.OnSubscription(svc, chr, deviceId, subscribed); });
                });
            });
            wrapper->server->SetReadyToSendCallback([st](const std::string& deviceId) {
                GuardVoid([&] { ForwardToLink(st, [&](Internal::LinkHooks& l) { l.OnReadyToSend(deviceId); }); });
            });
            wrapper->server->SetMaxPayloadCallback([st](const std::string& deviceId, size_t maxPayload) {
                GuardVoid([&] {
                    ForwardToLink(st, [&](Internal::LinkHooks& l) { l.OnMaxPayload(deviceId, maxPayload); });
                });
            });

            const bool wasInitialized = wrapper->server->IsInitialized();
            st->SetActive(true);
            auto result = wrapper->server->Initialize(nativeConfig);
            if (result != BleError::Success) {
                if (!wasInitialized) st->SetActive(false);
                st->SetLastError("Initialize failed with error code " + std::to_string(static_cast<int>(result)));
            }
            return static_cast<int>(result);
        });
    }

    BLE_API void BleServer_Shutdown(void* server) {
        GuardVoid([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper) return;
            std::lock_guard<std::mutex> lifecycle(wrapper->lifecycleMutex);
            wrapper->state->DeactivateAndDrain();  // no new callbacks; running ones have returned
            ShutdownLink(*wrapper->state);         // BleLink: fail pending messages, send CANCEL
            wrapper->server->Shutdown();           // idempotent in every backend
        });
    }

    BLE_API bool BleServer_IsInitialized(void* server) {
        return GuardRet(false, [&] {
            auto wrapper = Servers().Find(server);
            return wrapper ? wrapper->server->IsInitialized() : false;
        });
    }

    BLE_API int BleServer_GetApiVersion() {
        return kApiVersion;
    }

    BLE_API const char* BleServer_GetBuildInfo() {
        // BLE_BUILD_PLATFORM is supplied by CMake (windows / macos / linux / fake).
        return "BluetoothServer native plugin v1.0.0 api=3 platform=" BLE_BUILD_PLATFORM;
    }

    BLE_API const char* BleServer_GetLastError(void* server) {
        return GuardRet<const char*>("Internal error", [&]() -> const char* {
            auto wrapper = Servers().Find(server);
            if (!wrapper) return "Invalid server";
            // Pointer stays valid until the next error is recorded on this server.
            std::lock_guard<std::mutex> lock(wrapper->state->mutex);
            return wrapper->state->lastError.c_str();
        });
    }

    BLE_API uint32_t BleServer_GetCapabilities(void* server) {
        return GuardRet<uint32_t>(0, [&] {
            auto wrapper = Servers().Find(server);
            return wrapper ? wrapper->server->GetCapabilities() : 0u;
        });
    }

    BLE_API int BleServer_GetStatus(void* server, char* buffer, int* bufferLength) {
        return GuardInt([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper || !bufferLength) return Err(BleError::InvalidParameter);
            return CopyOut(wrapper->server->DescribeStatus(), buffer, bufferLength);
        });
    }

    BLE_API void BleServer_LogStatus(void* server) {
        GuardVoid([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper) {
                std::fprintf(stderr, "[BluetoothServer] LogStatus called with invalid server\n");
                return;
            }
            std::fprintf(stderr, "[BluetoothServer] %s\n", wrapper->server->DescribeStatus().c_str());
        });
    }

    BLE_API int BleServer_AddService(void* server, const char* serviceUuid, bool isPrimary) {
        return GuardInt([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper || !ValidUuid(serviceUuid)) return Err(BleError::InvalidParameter);

            BleServer::ServiceConfig config;
            config.uuid = serviceUuid;
            config.isPrimary = isPrimary;
            return static_cast<int>(wrapper->server->AddService(config));
        });
    }

    BLE_API int BleServer_RemoveService(void* server, const char* serviceUuid) {
        return GuardInt([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper || !ValidUuid(serviceUuid)) return Err(BleError::InvalidParameter);
            return static_cast<int>(wrapper->server->RemoveService(serviceUuid));
        });
    }

    BLE_API int BleServer_AddCharacteristic(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        uint32_t properties,
        uint32_t permissions) {
        return GuardInt([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper || !ValidUuid(serviceUuid) || !ValidUuid(characteristicUuid)) {
                return Err(BleError::InvalidParameter);
            }
            auto config = ToCharacteristicConfig(characteristicUuid, properties, permissions, nullptr, 0, 512);
            return static_cast<int>(wrapper->server->AddCharacteristic(serviceUuid, config));
        });
    }

    BLE_API int BleServer_AddCharacteristicEx(void* server,
        const char* serviceUuid,
        const BleCharacteristicConfigC* config) {
        return GuardInt([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper || !config || !ValidUuid(serviceUuid) || !ValidUuid(config->uuid)) {
                return Err(BleError::InvalidParameter);
            }
            auto nativeConfig = ToCharacteristicConfig(
                config->uuid,
                config->properties,
                config->permissions,
                config->initialValue,
                config->initialValueLength,
                config->maxLength);
            return static_cast<int>(wrapper->server->AddCharacteristic(serviceUuid, nativeConfig));
        });
    }

    BLE_API int BleServer_RemoveCharacteristic(void* server,
        const char* serviceUuid,
        const char* characteristicUuid) {
        return GuardInt([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper || !ValidUuid(serviceUuid) || !ValidUuid(characteristicUuid)) {
                return Err(BleError::InvalidParameter);
            }
            return static_cast<int>(wrapper->server->RemoveCharacteristic(serviceUuid, characteristicUuid));
        });
    }

    BLE_API int BleServer_AddDescriptor(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        const BleDescriptorConfigC* config) {
        return GuardInt([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper || !config || !ValidUuid(serviceUuid) || !ValidUuid(characteristicUuid) ||
                !ValidUuid(config->uuid)) {
                return Err(BleError::InvalidParameter);
            }

            BleServer::DescriptorConfig nativeConfig;
            nativeConfig.uuid = config->uuid;
            nativeConfig.permissions = config->permissions;
            if (config->initialValue && config->initialValueLength > 0) {
                nativeConfig.initialValue.assign(config->initialValue, config->initialValue + config->initialValueLength);
            }
            return static_cast<int>(wrapper->server->AddDescriptor(serviceUuid, characteristicUuid, nativeConfig));
        });
    }

    BLE_API void BleServer_SetConnectionCallback(void* server, ConnectionCallbackFn callback) {
        GuardVoid([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper) return;
            std::lock_guard<std::mutex> lock(wrapper->state->mutex);
            wrapper->state->connectionCallback = callback;
        });
    }

    BLE_API void BleServer_SetConnectionCallbackEx(void* server, ConnectionCallbackExFn callback, void* userData) {
        GuardVoid([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper) return;
            std::lock_guard<std::mutex> lock(wrapper->state->mutex);
            wrapper->state->connectionCallbackEx = callback;
            wrapper->state->connectionUserData = userData;
        });
    }

    BLE_API void BleServer_SetErrorCallback(void* server, ErrorCallbackFn callback) {
        GuardVoid([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper) return;
            std::lock_guard<std::mutex> lock(wrapper->state->mutex);
            wrapper->state->errorCallback = callback;
        });
    }

    BLE_API void BleServer_SetErrorCallbackEx(void* server, ErrorCallbackExFn callback, void* userData) {
        GuardVoid([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper) return;
            std::lock_guard<std::mutex> lock(wrapper->state->mutex);
            wrapper->state->errorCallbackEx = callback;
            wrapper->state->errorUserData = userData;
        });
    }

    BLE_API int BleServer_SetCharacteristicReadCallback(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        ReadCallbackFn callback,
        void* userData) {
        return GuardInt([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper || !ValidUuid(serviceUuid) || !ValidUuid(characteristicUuid)) {
                return Err(BleError::InvalidParameter);
            }

            std::shared_ptr<CallbackState> st = wrapper->state;
            auto readCallback = [st, callback, userData](const std::string& deviceId) {
                std::vector<uint8_t> data;
                if (!callback) return data;
                CallbackScope scope(*st);
                if (!scope.entered()) return data;

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

            return static_cast<int>(wrapper->server->SetCharacteristicReadCallback(serviceUuid, characteristicUuid, readCallback));
        });
    }

    BLE_API void BleServer_SetCharacteristicWriteCallback(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        DataCallbackFn callback) {
        GuardVoid([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper || !ValidUuid(serviceUuid) || !ValidUuid(characteristicUuid)) return;

            std::shared_ptr<CallbackState> st = wrapper->state;
            auto writeCallback = [st, callback](const std::string& deviceId, const std::vector<uint8_t>& data) {
                if (!callback) return;
                CallbackScope scope(*st);
                if (scope.entered()) callback(deviceId.c_str(), data.data(), static_cast<int>(data.size()));
            };
            wrapper->server->SetCharacteristicWriteCallback(serviceUuid, characteristicUuid, writeCallback);
        });
    }

    BLE_API int BleServer_SetCharacteristicWriteCallbackEx(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        DataCallbackExFn callback,
        void* userData) {
        return GuardInt([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper || !ValidUuid(serviceUuid) || !ValidUuid(characteristicUuid)) {
                return Err(BleError::InvalidParameter);
            }

            std::shared_ptr<CallbackState> st = wrapper->state;
            auto writeCallback = [st, callback, userData](const std::string& deviceId, const std::vector<uint8_t>& data) {
                if (!callback) return;
                CallbackScope scope(*st);
                if (scope.entered()) callback(deviceId.c_str(), data.data(), static_cast<int>(data.size()), userData);
            };
            return static_cast<int>(wrapper->server->SetCharacteristicWriteCallback(serviceUuid, characteristicUuid, writeCallback));
        });
    }

    BLE_API int BleServer_StartAdvertising(void* server) {
        return GuardInt([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper) return Err(BleError::InvalidParameter);
            return static_cast<int>(wrapper->server->StartAdvertising());
        });
    }

    BLE_API int BleServer_StopAdvertising(void* server) {
        return GuardInt([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper) return Err(BleError::InvalidParameter);
            return static_cast<int>(wrapper->server->StopAdvertising());
        });
    }

    BLE_API bool BleServer_IsAdvertising(void* server) {
        return GuardRet(false, [&] {
            auto wrapper = Servers().Find(server);
            return wrapper ? wrapper->server->IsAdvertising() : false;
        });
    }

    BLE_API int BleServer_UpdateCharacteristicValue(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        const uint8_t* value,
        int valueLength) {
        return GuardInt([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper || !ValidUuid(serviceUuid) || !ValidUuid(characteristicUuid) || !value || valueLength < 0) {
                return Err(BleError::InvalidParameter);
            }
            std::vector<uint8_t> data(value, value + valueLength);
            return static_cast<int>(wrapper->server->UpdateCharacteristicValue(serviceUuid, characteristicUuid, data));
        });
    }

    BLE_API int BleServer_NotifyCharacteristicChanged(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        const char* deviceId) {
        return GuardInt([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper || !ValidUuid(serviceUuid) || !ValidUuid(characteristicUuid)) {
                return Err(BleError::InvalidParameter);
            }
            std::string device = deviceId ? deviceId : "";
            return static_cast<int>(wrapper->server->NotifyCharacteristicChanged(serviceUuid, characteristicUuid, device));
        });
    }

    BLE_API int BleServer_GetConnectedDeviceCount(void* server) {
        return GuardRet(-1, [&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper) return -1;
            return static_cast<int>(wrapper->server->GetConnectedDevices().size());
        });
    }

    BLE_API int BleServer_GetConnectedDeviceId(void* server, int index, char* buffer, int* bufferLength) {
        return GuardInt([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper || index < 0 || !bufferLength) return Err(BleError::InvalidParameter);

            auto devices = wrapper->server->GetConnectedDevices();
            if (index >= static_cast<int>(devices.size())) return Err(BleError::InvalidParameter);
            return CopyOut(devices[static_cast<size_t>(index)], buffer, bufferLength);
        });
    }

    BLE_API int BleServer_DisconnectDevice(void* server, const char* deviceId) {
        return GuardInt([&] {
            auto wrapper = Servers().Find(server);
            if (!wrapper || !deviceId) return Err(BleError::InvalidParameter);
            return static_cast<int>(wrapper->server->DisconnectDevice(deviceId));
        });
    }

    // ---------------------------------------------------------------- Chunker C API
    BLE_API void* BleChunker_Create() {
        try {
            return Chunkers().Add(std::make_shared<BleChunkerWrapper>());
        }
        catch (...) {
            return nullptr;
        }
    }

    BLE_API void BleChunker_Destroy(void* chunker) {
        GuardVoid([&] { Chunkers().Remove(chunker); });
    }

    BLE_API int BleChunker_SendMessage(void* chunker, void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        const uint8_t* data,
        int dataLength) {
        return GuardInt([&] {
            auto chunkerWrapper = Chunkers().Find(chunker);
            auto wrapper = Servers().Find(server);
            if (!chunkerWrapper || !wrapper || !ValidUuid(serviceUuid) || !ValidUuid(characteristicUuid) ||
                dataLength < 0 || (!data && dataLength > 0)) {
                return Err(BleError::InvalidParameter);
            }

            std::vector<uint8_t> message;
            if (dataLength > 0) message.assign(data, data + dataLength);
            auto chunks = BleMessageChunker::CreateChunks(message);
            if (chunks.empty()) {
                return Err(BleError::InvalidParameter);  // larger than MAX_MESSAGE_SIZE
            }

            for (const auto& chunk : chunks) {
                auto result = wrapper->server->UpdateCharacteristicValue(serviceUuid, characteristicUuid, chunk);
                if (result != BleError::Success) {
                    return static_cast<int>(result);
                }
                // Notify all connected devices
                wrapper->server->NotifyCharacteristicChanged(serviceUuid, characteristicUuid, "");
            }

            // Remember the message so lost ACKs can be retransmitted (bounded: oldest is dropped).
            OutstandingMessage pending;
            pending.messageId = BleMessageChunker::GetMessageId(chunks.front());
            pending.server = wrapper;
            pending.serviceUuid = serviceUuid;
            pending.characteristicUuid = characteristicUuid;
            pending.acked.assign(chunks.size(), false);
            pending.retries.assign(chunks.size(), 0);
            pending.chunks = std::move(chunks);
            pending.lastSent = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(chunkerWrapper->mutex);
                chunkerWrapper->outstanding.push_back(std::move(pending));
                while (chunkerWrapper->outstanding.size() > Internal::kMaxOutstandingMessages) {
                    chunkerWrapper->outstanding.pop_front();
                }
            }
            return Err(BleError::Success);
        });
    }

    BLE_API bool BleChunker_ProcessReceivedData(void* chunker,
        const uint8_t* data,
        int dataLength,
        uint8_t* completeMessage,
        int* completeMessageLength) {
        return GuardRet(false, [&] {
            auto wrapper = Chunkers().Find(chunker);
            if (!wrapper || !data || dataLength < 0) return false;

            std::lock_guard<std::mutex> lock(wrapper->mutex);

            std::vector<uint8_t> chunk(data, data + dataLength);
            std::vector<uint8_t> complete;
            if (IngestPacket(*wrapper, chunk, complete)) {
                PushComplete(*wrapper, std::move(complete));
            }

            if (!wrapper->completeMessages.empty()) {
                auto& msg = wrapper->completeMessages.front();
                if (completeMessage && completeMessageLength && *completeMessageLength >= static_cast<int>(msg.size())) {
                    if (!msg.empty()) std::memcpy(completeMessage, msg.data(), msg.size());
                    *completeMessageLength = static_cast<int>(msg.size());
                    wrapper->completeMessages.pop();
                    return true;
                }
            }
            return false;
        });
    }

    BLE_API int BleChunker_ProcessIncoming(void* chunker,
        const uint8_t* data,
        int dataLength,
        uint8_t* ackBuffer,
        int* ackLength) {
        return GuardInt([&] {
            auto wrapper = Chunkers().Find(chunker);
            if (!wrapper || !data || dataLength < 0) return Err(BleError::InvalidParameter);

            std::vector<uint8_t> packet(data, data + dataLength);
            if (!BleMessageChunker::IsValidChunk(packet)) return Err(BleError::InvalidParameter);

            std::lock_guard<std::mutex> lock(wrapper->mutex);

            if (BleMessageChunker::IsAck(packet)) {
                // Idempotent: duplicate / out-of-order / unknown ACKs are harmless.
                const uint32_t id = BleMessageChunker::GetMessageId(packet);
                const uint32_t index = BleMessageChunker::GetChunkIndex(packet);
                for (auto it = wrapper->outstanding.begin(); it != wrapper->outstanding.end(); ++it) {
                    if (it->messageId != id) continue;
                    if (index < it->acked.size()) it->acked[index] = true;
                    if (std::all_of(it->acked.begin(), it->acked.end(), [](bool b) { return b; })) {
                        wrapper->outstanding.erase(it);  // fully acknowledged: release memory
                    }
                    break;
                }
                return Err(BleError::Success);
            }

            const int ackCapacity = ackLength ? *ackLength : 0;
            auto ack = BleMessageChunker::CreateAck(
                BleMessageChunker::GetMessageId(packet),
                BleMessageChunker::GetChunkIndex(packet));

            if (ackLength) {
                const int requiredLength = static_cast<int>(ack.size());
                if (ackBuffer && ackCapacity >= requiredLength) {
                    std::memcpy(ackBuffer, ack.data(), ack.size());
                }
                *ackLength = requiredLength;
            }

            std::vector<uint8_t> complete;
            if (IngestPacket(*wrapper, packet, complete)) {
                PushComplete(*wrapper, std::move(complete));
                return 1;
            }
            return 0;
        });
    }

    BLE_API int BleChunker_GetNextMessageSize(void* chunker) {
        return GuardRet(-1, [&] {
            auto wrapper = Chunkers().Find(chunker);
            if (!wrapper) return -1;
            std::lock_guard<std::mutex> lock(wrapper->mutex);
            if (wrapper->completeMessages.empty()) return 0;
            return static_cast<int>(wrapper->completeMessages.front().size());
        });
    }

    BLE_API bool BleChunker_ReadNextMessage(void* chunker,
        uint8_t* completeMessage,
        int* completeMessageLength) {
        return GuardRet(false, [&] {
            auto wrapper = Chunkers().Find(chunker);
            if (!wrapper || !completeMessageLength) return false;

            std::lock_guard<std::mutex> lock(wrapper->mutex);
            if (wrapper->completeMessages.empty()) {
                *completeMessageLength = 0;
                return false;
            }

            auto& msg = wrapper->completeMessages.front();
            const int requiredLength = static_cast<int>(msg.size());
            // An empty message needs no buffer (callers may pass null with length 0).
            if ((!completeMessage && requiredLength > 0) || *completeMessageLength < requiredLength) {
                *completeMessageLength = requiredLength;
                return false;
            }

            if (!msg.empty()) {
                std::memcpy(completeMessage, msg.data(), msg.size());
            }
            *completeMessageLength = requiredLength;
            wrapper->completeMessages.pop();
            return true;
        });
    }

    // Retransmission contract: every sent message with an unacknowledged chunk whose last
    // transmission is at least timeoutMs old gets its OLDEST unacknowledged chunk resent. After
    // kMaxChunkRetries resends of one chunk the message is failed: it is released and reported
    // through the server's error callback (BleError::Unknown, "chunk N not acknowledged").
    // Returns the number of chunks resent (>= 0), or InvalidParameter (4) for an invalid handle or
    // a negative timeout.
    BLE_API int BleChunker_CleanupTimedOut(void* chunker, int timeoutMs) {
        return GuardInt([&] {
            auto wrapper = Chunkers().Find(chunker);
            if (!wrapper || timeoutMs < 0) return Err(BleError::InvalidParameter);

            struct Resend {
                std::shared_ptr<BleServerWrapper> server;
                std::string svc, chr;
                std::vector<uint8_t> chunk;
            };
            struct Failure {
                std::shared_ptr<BleServerWrapper> server;
                std::string message;
            };
            std::vector<Resend> resends;
            std::vector<Failure> failures;

            {
                const auto now = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lock(wrapper->mutex);
                for (auto it = wrapper->outstanding.begin(); it != wrapper->outstanding.end();) {
                    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->lastSent).count();
                    if (age < timeoutMs) { ++it; continue; }

                    size_t idx = 0;
                    while (idx < it->acked.size() && it->acked[idx]) ++idx;
                    if (idx >= it->acked.size()) { it = wrapper->outstanding.erase(it); continue; }

                    auto owner = it->server.lock();
                    if (!owner) { it = wrapper->outstanding.erase(it); continue; }  // server destroyed

                    if (it->retries[idx] >= Internal::kMaxChunkRetries) {
                        failures.push_back({owner, "chunk " + std::to_string(idx) + " not acknowledged"});
                        it = wrapper->outstanding.erase(it);
                        continue;
                    }
                    it->retries[idx]++;
                    it->lastSent = now;
                    resends.push_back({owner, it->serviceUuid, it->characteristicUuid, it->chunks[idx]});
                    ++it;
                }
            }  // chunker lock released: backend and user callbacks run unlocked

            int sent = 0;
            for (auto& r : resends) {
                if (r.server->server->UpdateCharacteristicValue(r.svc, r.chr, r.chunk) == BleError::Success) {
                    r.server->server->NotifyCharacteristicChanged(r.svc, r.chr, "");
                    ++sent;
                }
            }
            for (auto& f : failures) {
                DeliverError(f.server->state, BleError::Unknown, f.message);
            }
            return sent;
        });
    }

} // extern "C"
