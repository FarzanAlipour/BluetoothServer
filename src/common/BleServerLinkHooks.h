// BleServerLinkHooks.h - how the BleLink C ABI (BleLinkAbi.cpp) attaches to a BleServer C handle
// (BleServer.cpp). Internal; not part of the public ABI.
//
// A server handle owns the backend callbacks (connection, subscription, ready-to-send, max payload);
// it forwards them to at most one attached link. Forwarding runs on the backend's thread with no lock
// held and is gated like user callbacks (nothing after Shutdown/Destroy). Shutdown/Destroy detach the
// link and call OnServerShutdown() BEFORE the backend is shut down, so the link can still send CANCEL.
#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace BleServer {

class IBleServer;

namespace Internal {

    class LinkHooks {
    public:
        virtual ~LinkHooks() = default;
        virtual void OnConnection(const std::string& deviceId, bool connected) = 0;
        virtual void OnSubscription(const std::string& serviceUuid, const std::string& characteristicUuid,
            const std::string& deviceId, bool subscribed) = 0;
        virtual void OnReadyToSend(const std::string& deviceId) = 0;
        virtual void OnMaxPayload(const std::string& deviceId, size_t maxPayload) = 0;
        // The server is being shut down or destroyed; the link is already detached.
        virtual void OnServerShutdown() = 0;
    };

    struct ServerAccess {
        std::shared_ptr<void> keepAlive;  // keeps the server wrapper (and its backend object) alive
        IBleServer* server = nullptr;
    };

    // Resolves a live server handle. False for null/unknown/destroyed handles.
    bool AcquireServer(void* serverHandle, ServerAccess& out);
    // Attaches `hooks` to the server. False if the handle is invalid, the server is not initialized or
    // another link is attached.
    bool AttachLink(void* serverHandle, const std::shared_ptr<LinkHooks>& hooks);
    // Detaches `hooks` if it is the attached link (no-op otherwise).
    void DetachLink(void* serverHandle, const LinkHooks* hooks);
    // Records a message for BleServer_GetLastError(serverHandle).
    void SetServerLastError(void* serverHandle, const std::string& message);

    // Number of live BleLink handles (leak check in tests).
    size_t LiveLinkHandles();

} // namespace Internal
} // namespace BleServer
