// BleLinkAbi.cpp - C ABI (API v3) for the BleLink v1 engine (docs/PROTOCOL.md, docs/BLELINK_ABI.md).
//
// One link per server handle. The link registers the BleLink service (RX write, TX notify, INFO read)
// on the backend, runs a BleLink::Engine (Role::Server) on its own worker thread and feeds it from the
// backend: RX writes -> OnPacket, TX subscription -> OnPeerSubscribed/Unsubscribed, connection ->
// OnPeerConnected/Disconnected, ready-to-send -> OnTransportReady, max payload -> SetPeerOsLimit. The
// engine sends through IBleServer::NotifyPeer (one notification to one central, Busy = flow control).
// Events are polled by the application (BleLink_PollEvent); no callback into managed code.
//
// Same rules as BleServer.cpp: handles are validated through a registry, each call holds a shared_ptr
// (a concurrent Destroy cannot free the link under it), no exception crosses the boundary, nothing is
// delivered into the engine after Shutdown/Destroy (the engine rejects inputs once shut down).
#include "IBleServer.h"
#include "BleLinkCodec.h"
#include "BleLinkEngine.h"
#include "BleServerLinkHooks.h"
#include "BleUtilities.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using BleServer::BleError;
using BleServer::IBleServer;
namespace Internal = BleServer::Internal;
namespace Link = BleServer::BleLink;

constexpr int Err(BleError e) { return static_cast<int>(e); }

template <class F> int GuardInt(F&& f) {
    try { return f(); } catch (...) { return Err(BleError::Unknown); }
}
template <class F> void GuardVoid(F&& f) {
    try { f(); } catch (...) {}
}

// ---- C struct layout: what unity/BleServer.cs mirrors (also checked by scripts/check_abi_parity.py) ----
static_assert(sizeof(BleLinkConfigC) == 24, "BleLinkConfigC layout");
static_assert(sizeof(BleLinkPeerInfoC) == 56, "BleLinkPeerInfoC layout");
static_assert(offsetof(BleLinkEventC, bytes) == 8, "BleLinkEventC layout");
static_assert(offsetof(BleLinkEventC, messageId) == 24, "BleLinkEventC layout");
static_assert(offsetof(BleLinkEventC, dataLength) == 44, "BleLinkEventC layout");
static_assert(offsetof(BleLinkEventC, info) == 48, "BleLinkEventC layout");
static_assert(offsetof(BleLinkEventC, deviceIdLength) == 104, "BleLinkEventC layout");
static_assert(offsetof(BleLinkEventC, deviceId) == 108, "BleLinkEventC layout");
static_assert(offsetof(BleLinkEventC, reserved) == 364, "BleLinkEventC layout");
static_assert(sizeof(BleLinkEventC) == 368, "BleLinkEventC layout");

constexpr uint32_t kKnownFlags = BLELINK_FLAG_NO_PROGRESS | BLELINK_FLAG_ENCRYPTED;
constexpr uint32_t kDefaultMaxMessage = 16u * 1024u * 1024u;
constexpr uint32_t kMaxAbiMessage = 0x7FFFFFFFu;  // payload sizes are `int` in this ABI
constexpr uint32_t kMaxTimeoutMs = 255u * 250u;

struct LinkSettings {
    Link::Config engine;
    bool encrypted = false;
};

// Reads the fields the caller's struct version has (size), validates them. Empty string = OK.
std::string ParseConfig(const BleLinkConfigC* c, LinkSettings& out) {
    out.engine.role = Link::Role::Server;
    out.engine.maxMessage = kDefaultMaxMessage;
    out.engine.window = 4;
    out.engine.maxPacket = Link::kMaxPacketSize;
    out.engine.timeoutUnits = 0;
    out.engine.wantProgress = true;
    if (!c) return {};
    if (c->size < sizeof(uint32_t)) return "BleLinkConfigC.size is 0 (set it to sizeof(BleLinkConfigC))";
    auto has = [&](size_t offset) { return c->size >= offset + sizeof(uint32_t); };

    if (has(offsetof(BleLinkConfigC, maxMessage)) && c->maxMessage != 0) {
        if (c->maxMessage > kMaxAbiMessage) return "maxMessage must be 1..0x7FFFFFFF";
        out.engine.maxMessage = c->maxMessage;
    }
    if (has(offsetof(BleLinkConfigC, window)) && c->window != 0) {
        if (c->window > 255) return "window must be 1..255";
        out.engine.window = static_cast<uint8_t>(c->window);
    }
    if (has(offsetof(BleLinkConfigC, maxPacket)) && c->maxPacket != 0) {
        if (c->maxPacket < Link::kMinPacketSize || c->maxPacket > Link::kMaxPacketSize)
            return "maxPacket must be 20..512";
        out.engine.maxPacket = static_cast<uint16_t>(c->maxPacket);
    }
    if (has(offsetof(BleLinkConfigC, timeoutMs)) && c->timeoutMs != 0) {
        if (c->timeoutMs > kMaxTimeoutMs) return "timeoutMs must be 1..63750";
        out.engine.timeoutUnits = static_cast<uint8_t>((c->timeoutMs + 249u) / 250u);
    }
    if (has(offsetof(BleLinkConfigC, flags))) {
        if (c->flags & ~kKnownFlags) return "unknown BleLinkConfigC.flags bits";
        out.engine.wantProgress = (c->flags & BLELINK_FLAG_NO_PROGRESS) == 0;
        out.encrypted = (c->flags & BLELINK_FLAG_ENCRYPTED) != 0;
    }
    return {};
}

bool SameUuid(const std::string& a, const char* b) {
    return BleServer::Utils::NormalizeUuid(a) == BleServer::Utils::NormalizeUuid(b);
}

uint16_t OsLimitFromPayload(size_t maxPayload) {
    if (maxPayload == 0) return 0;
    if (maxPayload < Link::kMinPacketSize) return Link::kMinPacketSize;
    if (maxPayload > Link::kMaxPacketSize) return Link::kMaxPacketSize;
    return static_cast<uint16_t>(maxPayload);
}

int32_t ToC(Link::FailReason r) {
    switch (r) {
    case Link::FailReason::Disconnected: return BLELINK_REASON_DISCONNECTED;
    case Link::FailReason::Reset: return BLELINK_REASON_RESET;
    case Link::FailReason::Timeout: return BLELINK_REASON_TIMEOUT;
    case Link::FailReason::Crc: return BLELINK_REASON_CRC;
    case Link::FailReason::TooLarge: return BLELINK_REASON_TOO_LARGE;
    case Link::FailReason::Busy: return BLELINK_REASON_BUSY;
    case Link::FailReason::Rejected: return BLELINK_REASON_REJECTED;
    case Link::FailReason::LinkUnreliable: return BLELINK_REASON_LINK_UNRELIABLE;
    case Link::FailReason::Cancelled: return BLELINK_REASON_CANCELLED;
    case Link::FailReason::NotReady: return BLELINK_REASON_NOT_READY;
    case Link::FailReason::Shutdown: return BLELINK_REASON_SHUTDOWN;
    }
    return BLELINK_REASON_NONE;
}

int32_t ToC(Link::EventType t) {
    switch (t) {
    case Link::EventType::PeerReady: return BLELINK_EVENT_PEER_READY;
    case Link::EventType::PeerGone: return BLELINK_EVENT_PEER_GONE;
    case Link::EventType::MessageReceived: return BLELINK_EVENT_MESSAGE_RECEIVED;
    case Link::EventType::MessageSent: return BLELINK_EVENT_MESSAGE_SENT;
    case Link::EventType::MessageFailed: return BLELINK_EVENT_MESSAGE_FAILED;
    case Link::EventType::Progress: return BLELINK_EVENT_PROGRESS;
    }
    return BLELINK_EVENT_NONE;
}

int32_t ToC(Link::ProgressKind k) {
    switch (k) {
    case Link::ProgressKind::Sending: return BLELINK_PROGRESS_SENDING;
    case Link::ProgressKind::PeerReceived: return BLELINK_PROGRESS_PEER_RECEIVED;
    case Link::ProgressKind::Receiving: return BLELINK_PROGRESS_RECEIVING;
    }
    return BLELINK_PROGRESS_NONE;
}

void FillInfo(const Link::PeerInfo& in, BleLinkPeerInfoC& out) {
    out = BleLinkPeerInfoC{};
    out.size = sizeof(BleLinkPeerInfoC);
    out.subscribed = in.subscribed ? 1u : 0u;
    out.ready = in.ready ? 1u : 0u;
    out.txPacketSize = in.txPacketSize;
    out.osLimit = in.osLimit;
    out.peerVersion = in.peer.version;
    out.peerMaxPacket = in.peer.maxPacket;
    out.peerMaxMessage = in.peer.maxMessage;
    out.peerWindow = in.peer.window;
    out.peerFlags = in.peer.flags;
    out.timeoutMs = in.timeoutMs;
    out.queued = static_cast<uint32_t>(in.queued);
    out.inFlight = static_cast<uint32_t>(in.inFlight);
    out.receiving = static_cast<uint32_t>(in.receiving);
}

// ---------------------------------------------------------------------------------------------------
class LinkCore;

// Engine -> backend. Called only from the engine's Pump()/Shutdown().
class NotifyTransport : public Link::ITransport {
public:
    NotifyTransport(IBleServer* server, LinkCore* owner) : server_(server), owner_(owner) {}
    Link::TransportResult SendPacket(const Link::PeerId& peer, const uint8_t* data, size_t len) override;
    void RequestDisconnect(const Link::PeerId& peer) override {
        try { server_->DisconnectDevice(peer); } catch (...) {}
    }

private:
    IBleServer* server_;
    LinkCore* owner_;
};

class LinkCore : public Internal::LinkHooks {
public:
    LinkCore(void* serverHandle, Internal::ServerAccess access, const LinkSettings& settings)
        : serverHandle_(serverHandle),
          access_(std::move(access)),
          settings_(settings),
          transport_(access_.server, this),
          engine_(transport_, settings.engine) {}

    ~LinkCore() override { engine_.Shutdown(); }

    IBleServer& Server() { return *access_.server; }
    Link::Engine& Engine() { return engine_; }
    void* ServerHandle() const { return serverHandle_; }
    const LinkSettings& Settings() const { return settings_; }

    // ---- LinkHooks (backend thread, no lock held, must not block) ----
    void OnConnection(const std::string& deviceId, bool connected) override {
        if (deviceId.empty()) return;
        if (connected) {
            engine_.OnPeerConnected(deviceId);
            return;
        }
        // macOS reports "connected=false" whenever a central unsubscribes from ANY characteristic.
        // Only end the session if this central is not (or no longer) subscribed to TX.
        if (IsTxSubscriber(deviceId)) return;
        engine_.OnPeerDisconnected(deviceId);
    }

    void OnSubscription(const std::string& serviceUuid, const std::string& characteristicUuid,
        const std::string& deviceId, bool subscribed) override {
        if (deviceId.empty() || !SameUuid(serviceUuid, BLELINK_SERVICE_UUID) ||
            !SameUuid(characteristicUuid, BLELINK_TX_UUID)) {
            return;
        }
        if (subscribed) {
            const uint16_t limit = OsLimitFromPayload(Server().GetMaxNotifyPayload(deviceId));
            if (limit) engine_.SetPeerOsLimit(deviceId, limit);
            engine_.OnPeerSubscribed(deviceId);
        } else if (IsConnected(deviceId)) {
            engine_.OnPeerUnsubscribed(deviceId);  // session closed, the central may subscribe again
        } else {
            engine_.OnPeerDisconnected(deviceId);  // disconnect reported before/without connection(false)
        }
    }

    void OnReadyToSend(const std::string& deviceId) override { engine_.OnTransportReady(deviceId); }

    void OnMaxPayload(const std::string& deviceId, size_t maxPayload) override {
        const uint16_t limit = OsLimitFromPayload(maxPayload);
        if (!deviceId.empty() && limit) engine_.SetPeerOsLimit(deviceId, limit);
    }

    void OnServerShutdown() override {
        serverGone_.store(true);
        engine_.Shutdown();  // fails pending messages (events stay pollable), CANCEL best effort
    }

    // RX write from the backend (BLE thread).
    void OnRxWrite(const std::string& deviceId, const std::vector<uint8_t>& data) {
        if (deviceId.empty()) return;
        engine_.OnPacket(deviceId, data.data(), data.size());
    }

    // TooLarge from NotifyPeer: the OS limit dropped; follow it (backend input, safe from SendPacket).
    void OnTooLarge(const std::string& deviceId) {
        const uint16_t limit = OsLimitFromPayload(Server().GetMaxNotifyPayload(deviceId));
        if (limit) engine_.SetPeerOsLimit(deviceId, limit);
    }

    bool ServerGone() const { return serverGone_.load(); }

    // Event peek slot: BleLink_GetEventPayloadSize looks at the next event without taking it.
    std::mutex eventMutex;
    bool hasPeeked = false;
    Link::Event peeked;

    // Ensures `peeked` holds the next event. Caller holds eventMutex.
    bool PeekLocked() {
        if (hasPeeked) return true;
        hasPeeked = engine_.PollEvent(peeked);
        return hasPeeked;
    }

private:
    bool IsConnected(const std::string& deviceId) {
        for (const auto& id : Server().GetConnectedDevices()) {
            if (id == deviceId) return true;
        }
        return false;
    }

    bool IsTxSubscriber(const std::string& deviceId) {
        for (const auto& id : Server().GetSubscribers(BLELINK_SERVICE_UUID, BLELINK_TX_UUID)) {
            if (id == deviceId) return true;
        }
        return false;
    }

    void* serverHandle_;
    Internal::ServerAccess access_;
    LinkSettings settings_;
    NotifyTransport transport_;  // before engine_: the engine uses it until it is destroyed
    Link::Engine engine_;
    std::atomic<bool> serverGone_{false};
};

Link::TransportResult NotifyTransport::SendPacket(const Link::PeerId& peer, const uint8_t* data, size_t len) {
    try {
        switch (server_->NotifyPeer(BLELINK_SERVICE_UUID, BLELINK_TX_UUID, peer, data, len)) {
        case BleServer::NotifyStatus::Sent: return Link::TransportResult::Sent;
        case BleServer::NotifyStatus::Busy: return Link::TransportResult::Busy;
        case BleServer::NotifyStatus::TooLarge:
            owner_->OnTooLarge(peer);
            return Link::TransportResult::Error;
        default: return Link::TransportResult::Error;
        }
    } catch (...) {
        return Link::TransportResult::Error;
    }
}

// Public handle: owns the core; Destroy unregisters it.
struct LinkHandle {
    std::shared_ptr<LinkCore> core;
    std::mutex destroyMutex;
    bool destroyed = false;
};

class LinkRegistry {
public:
    void* Add(const std::shared_ptr<LinkHandle>& item) {
        std::lock_guard<std::mutex> lock(m_);
        items_[item.get()] = item;
        return item.get();
    }
    std::shared_ptr<LinkHandle> Find(const void* handle) {
        if (!handle) return nullptr;
        std::lock_guard<std::mutex> lock(m_);
        auto it = items_.find(handle);
        return it == items_.end() ? nullptr : it->second;
    }
    std::shared_ptr<LinkHandle> Remove(const void* handle) {
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
    std::map<const void*, std::shared_ptr<LinkHandle>> items_;
};

// Intentionally leaked (static destruction order), like the server registry.
LinkRegistry& Links() { static auto* r = new LinkRegistry(); return *r; }

std::shared_ptr<LinkCore> FindCore(void* link) {
    auto h = Links().Find(link);
    return h ? h->core : nullptr;
}

void SafeRemoveService(IBleServer& server) {
    try { server.RemoveService(BLELINK_SERVICE_UUID); } catch (...) {}
}

// Registers service + characteristics + callbacks. Empty string = OK.
std::string RegisterService(const std::shared_ptr<LinkCore>& core) {
    IBleServer& server = core->Server();
    const bool enc = core->Settings().encrypted;
    using BleServer::CharacteristicPermission;
    using BleServer::CharacteristicProperty;
    auto prop = [](CharacteristicProperty p) { return static_cast<uint32_t>(p); };
    auto perm = [](CharacteristicPermission p) { return static_cast<uint32_t>(p); };
    const uint32_t readPerm = perm(enc ? CharacteristicPermission::ReadEncrypted : CharacteristicPermission::Read);
    const uint32_t writePerm = perm(enc ? CharacteristicPermission::WriteEncrypted : CharacteristicPermission::Write);

    BleServer::ServiceConfig svc;
    svc.uuid = BLELINK_SERVICE_UUID;
    svc.isPrimary = true;
    BleError e = server.AddService(svc);
    if (e != BleError::Success) return "AddService(BleLink) failed with error " + std::to_string(static_cast<int>(e));

    BleServer::CharacteristicConfig rx;
    rx.uuid = BLELINK_RX_UUID;
    rx.properties = prop(CharacteristicProperty::Write) | prop(CharacteristicProperty::WriteWithoutResponse);
    rx.permissions = writePerm;
    rx.maxLength = Link::kMaxPacketSize;

    BleServer::CharacteristicConfig tx;
    tx.uuid = BLELINK_TX_UUID;
    tx.properties = prop(CharacteristicProperty::Notify);
    tx.permissions = readPerm;
    tx.maxLength = Link::kMaxPacketSize;

    BleServer::CharacteristicConfig info;
    info.uuid = BLELINK_INFO_UUID;
    info.properties = prop(CharacteristicProperty::Read);
    info.permissions = readPerm;
    info.initialValue = core->Engine().InfoValue();
    info.maxLength = 32;

    for (const auto* c : {&rx, &tx, &info}) {
        e = server.AddCharacteristic(BLELINK_SERVICE_UUID, *c);
        if (e != BleError::Success) {
            return "AddCharacteristic(" + c->uuid + ") failed with error " + std::to_string(static_cast<int>(e));
        }
    }

    std::weak_ptr<LinkCore> weak = core;
    e = server.SetCharacteristicWriteCallback(BLELINK_SERVICE_UUID, BLELINK_RX_UUID,
        [weak](const std::string& deviceId, const std::vector<uint8_t>& data) {
            GuardVoid([&] {
                if (auto c = weak.lock()) c->OnRxWrite(deviceId, data);
            });
        });
    if (e != BleError::Success) return "SetCharacteristicWriteCallback(RX) failed with error " + std::to_string(static_cast<int>(e));
    const std::vector<uint8_t> infoValue = core->Engine().InfoValue();
    e = server.SetCharacteristicReadCallback(BLELINK_SERVICE_UUID, BLELINK_INFO_UUID,
        [infoValue](const std::string&) { return infoValue; });
    if (e != BleError::Success) return "SetCharacteristicReadCallback(INFO) failed with error " + std::to_string(static_cast<int>(e));
    return {};
}

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

}  // namespace

namespace BleServer {
namespace Internal {
    size_t LiveLinkHandles() { return Links().Size(); }
}  // namespace Internal
}  // namespace BleServer

extern "C" {

BLE_API void* BleLink_Create(void* server, const BleLinkConfigC* config) {
    try {
        Internal::ServerAccess access;
        if (!Internal::AcquireServer(server, access)) return nullptr;

        LinkSettings settings;
        const std::string configError = ParseConfig(config, settings);
        if (!configError.empty()) {
            Internal::SetServerLastError(server, "BleLink_Create: " + configError);
            return nullptr;
        }
        if (!access.server->IsInitialized()) {
            Internal::SetServerLastError(server, "BleLink_Create: server is not initialized");
            return nullptr;
        }
        const uint32_t needed = static_cast<uint32_t>(BleServer::ServerCapability::PeerNotify);
        if ((access.server->GetCapabilities() & needed) == 0) {
            Internal::SetServerLastError(server, "BleLink_Create: backend has no per-central notify (PeerNotify)");
            return nullptr;
        }

        auto core = std::make_shared<LinkCore>(server, access, settings);
        if (!Internal::AttachLink(server, core)) {
            Internal::SetServerLastError(server,
                "BleLink_Create: a BleLink already exists on this server, or it is not initialized");
            return nullptr;
        }
        const std::string regError = RegisterService(core);
        if (!regError.empty()) {
            Internal::DetachLink(server, core.get());
            SafeRemoveService(core->Server());
            core->Engine().Shutdown();
            Internal::SetServerLastError(server, "BleLink_Create: " + regError);
            return nullptr;
        }
        core->Engine().Start();

        auto handle = std::make_shared<LinkHandle>();
        handle->core = core;
        return Links().Add(handle);
    } catch (...) {
        return nullptr;
    }
}

BLE_API void BleLink_Destroy(void* link) {
    GuardVoid([&] {
        auto handle = Links().Remove(link);  // second Destroy / stale handle: no-op
        if (!handle) return;
        std::lock_guard<std::mutex> lock(handle->destroyMutex);
        if (handle->destroyed) return;
        handle->destroyed = true;
        auto core = handle->core;
        Internal::DetachLink(core->ServerHandle(), core.get());  // no more backend events
        core->Engine().Shutdown();                               // CANCEL in-flight, stop worker
        if (!core->ServerGone()) {
            // The write/read callbacks hold only a weak_ptr; removing the service drops them.
            SafeRemoveService(core->Server());
        }
        // The core is freed when the last in-flight call releases its reference.
    });
}

BLE_API int BleLink_Send(void* link, const char* deviceId, const uint8_t* data, int dataLength,
    uint32_t* outMsgId) {
    return GuardInt([&] {
        if (outMsgId) *outMsgId = 0;
        auto core = FindCore(link);
        if (!core || !deviceId || !deviceId[0] || dataLength < 0 || (!data && dataLength > 0)) {
            return Err(BleError::InvalidParameter);
        }
        std::vector<uint8_t> message;
        if (dataLength > 0) message.assign(data, data + dataLength);
        const Link::SendOutcome out = core->Engine().Send(deviceId, std::move(message));
        switch (out.code) {
        case Link::SendOutcome::Code::Ok:
            if (outMsgId) *outMsgId = out.handle;
            return Err(BleError::Success);
        case Link::SendOutcome::Code::NotReady: return Err(BleError::PeerNotReady);
        case Link::SendOutcome::Code::TooLarge: return Err(BleError::MessageTooLarge);
        case Link::SendOutcome::Code::Stopped: return Err(BleError::NotInitialized);
        }
        return Err(BleError::Unknown);
    });
}

BLE_API int BleLink_Cancel(void* link, uint32_t msgId) {
    return GuardInt([&] {
        auto core = FindCore(link);
        if (!core || msgId == 0) return Err(BleError::InvalidParameter);
        core->Engine().Cancel(msgId);
        return Err(BleError::Success);
    });
}

BLE_API int BleLink_GetEventPayloadSize(void* link) {
    try {
        auto core = FindCore(link);
        if (!core) return -Err(BleError::InvalidParameter);
        std::lock_guard<std::mutex> lock(core->eventMutex);
        if (!core->PeekLocked()) return -1;
        return static_cast<int>(core->peeked.data.size());
    } catch (...) {
        return -Err(BleError::Unknown);
    }
}

BLE_API int BleLink_PollEvent(void* link, BleLinkEventC* ev, uint8_t* buffer, int bufferLength) {
    try {
        auto core = FindCore(link);
        if (!core || !ev || ev->size < sizeof(BleLinkEventC) || bufferLength < 0 ||
            (!buffer && bufferLength > 0)) {
            return -Err(BleError::InvalidParameter);
        }
        std::lock_guard<std::mutex> lock(core->eventMutex);
        if (!core->PeekLocked()) return 0;
        const Link::Event& e = core->peeked;

        const uint32_t callerSize = ev->size;
        std::memset(ev, 0, sizeof(BleLinkEventC));
        ev->size = callerSize;
        ev->type = ToC(e.type);
        ev->bytes = e.bytes;
        ev->total = e.total;
        ev->messageId = e.handle;
        ev->wireMsgId = e.msgId;
        if (e.type == Link::EventType::PeerGone || e.type == Link::EventType::MessageFailed) {
            ev->reason = ToC(e.reason);
            ev->ackStatus = e.ackStatus;
        }
        if (e.type == Link::EventType::Progress) ev->progressKind = ToC(e.progressKind);
        ev->dataLength = static_cast<int32_t>(e.data.size());
        FillInfo(e.info, ev->info);
        const size_t idLen = std::min(e.peer.size(), static_cast<size_t>(BLELINK_DEVICE_ID_CAPACITY - 1));
        std::memcpy(ev->deviceId, e.peer.data(), idLen);
        ev->deviceId[idLen] = '\0';
        ev->deviceIdLength = static_cast<uint32_t>(idLen);

        if (e.data.size() > static_cast<size_t>(bufferLength)) {
            return -Err(BleError::BufferTooSmall);  // event stays queued
        }
        if (!e.data.empty()) std::memcpy(buffer, e.data.data(), e.data.size());
        core->hasPeeked = false;
        core->peeked = Link::Event{};
        return 1;
    } catch (...) {
        return -Err(BleError::Unknown);
    }
}

BLE_API int BleLink_GetPeerInfo(void* link, const char* deviceId, BleLinkPeerInfoC* info) {
    return GuardInt([&] {
        auto core = FindCore(link);
        if (!core || !deviceId || !info || info->size < sizeof(BleLinkPeerInfoC)) {
            return Err(BleError::InvalidParameter);
        }
        Link::PeerInfo pi;
        if (!core->Engine().GetPeerInfo(deviceId, pi)) return Err(BleError::PeerNotReady);
        const uint32_t callerSize = info->size;
        FillInfo(pi, *info);
        info->size = callerSize;
        return Err(BleError::Success);
    });
}

BLE_API int BleLink_GetPeerCount(void* link) {
    try {
        auto core = FindCore(link);
        if (!core) return -1;
        return static_cast<int>(core->Engine().Peers().size());
    } catch (...) {
        return -1;
    }
}

BLE_API int BleLink_GetPeerId(void* link, int index, char* buffer, int* bufferLength) {
    return GuardInt([&] {
        auto core = FindCore(link);
        if (!core || index < 0 || !bufferLength) return Err(BleError::InvalidParameter);
        const auto peers = core->Engine().Peers();
        if (index >= static_cast<int>(peers.size())) return Err(BleError::InvalidParameter);
        return CopyOut(peers[static_cast<size_t>(index)], buffer, bufferLength);
    });
}

}  // extern "C"
