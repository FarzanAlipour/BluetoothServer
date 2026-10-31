#include "FakeBleServer.h"

namespace BleServer {

static std::atomic<FakeBleServer*> g_last{nullptr};

// Backend factory required by IBleServer::Create() (see src/common/BleServer.cpp).
std::unique_ptr<IBleServer> CreatePlatformServer() {
    return std::make_unique<FakeBleServer>();
}

FakeBleServer::FakeBleServer() { g_last = this; }
FakeBleServer::~FakeBleServer() {
    FakeBleServer* self = this;
    g_last.compare_exchange_strong(self, nullptr);
}
FakeBleServer* FakeBleServer::Last() { return g_last.load(); }

BleError FakeBleServer::Initialize(const ServerConfig&) {
    std::lock_guard<std::mutex> l(m_);
    if (initialized_) return BleError::AlreadyInitialized;
    initialized_ = true;
    return BleError::Success;
}

void FakeBleServer::Shutdown() {
    std::lock_guard<std::mutex> l(m_);
    initialized_ = false;
    advertising_ = false;
    devices_.clear();
    queue_.Clear();
    subscribers_.clear();
    gate_.Clear();
    peerCredits_.clear();
    maxPayload_.clear();
    if (retain_) return;
    services_.clear();
    writeCbs_.clear();
    readCbs_.clear();
    connCb_ = nullptr;
    errCb_ = nullptr;
    subCb_ = nullptr;
    readyCb_ = nullptr;
    maxPayloadCb_ = nullptr;
}

bool FakeBleServer::IsInitialized() const { std::lock_guard<std::mutex> l(m_); return initialized_; }

BleError FakeBleServer::AddService(const ServiceConfig& c) {
    std::lock_guard<std::mutex> l(m_);
    if (!initialized_) return BleError::NotInitialized;
    services_[c.uuid];
    return BleError::Success;
}

BleError FakeBleServer::RemoveService(const std::string& s) {
    std::lock_guard<std::mutex> l(m_);
    if (!initialized_) return BleError::NotInitialized;
    if (!services_.erase(s)) return BleError::ServiceNotFound;
    queue_.RemoveService(s);
    subscribers_.erase(s);
    writeCbs_.erase(s);
    readCbs_.erase(s);
    return BleError::Success;
}

std::vector<std::string> FakeBleServer::GetServices() const {
    std::lock_guard<std::mutex> l(m_);
    std::vector<std::string> out;
    for (auto& kv : services_) out.push_back(kv.first);
    return out;
}

BleError FakeBleServer::AddCharacteristic(const std::string& s, const CharacteristicConfig& c) {
    std::lock_guard<std::mutex> l(m_);
    if (!initialized_) return BleError::NotInitialized;
    auto it = services_.find(s);
    if (it == services_.end()) return BleError::ServiceNotFound;
    it->second[c.uuid] = c.initialValue;
    return BleError::Success;
}

BleError FakeBleServer::RemoveCharacteristic(const std::string& s, const std::string& c) {
    std::lock_guard<std::mutex> l(m_);
    if (!initialized_) return BleError::NotInitialized;
    auto it = services_.find(s);
    if (it == services_.end()) return BleError::ServiceNotFound;
    if (!it->second.erase(c)) return BleError::CharacteristicNotFound;
    queue_.RemoveCharacteristic(s, c);
    auto sv = subscribers_.find(s);
    if (sv != subscribers_.end()) sv->second.erase(c);
    return BleError::Success;
}

BleError FakeBleServer::SetCharacteristicReadCallback(const std::string& s, const std::string& c,
                                                      CharacteristicReadCallback cb) {
    std::lock_guard<std::mutex> l(m_);
    auto it = services_.find(s);
    if (it == services_.end()) return BleError::ServiceNotFound;
    if (!it->second.count(c)) return BleError::CharacteristicNotFound;
    readCbs_[s][c] = std::move(cb);
    return BleError::Success;
}

BleError FakeBleServer::SetCharacteristicWriteCallback(const std::string& s, const std::string& c,
                                                       CharacteristicWriteCallback cb) {
    std::lock_guard<std::mutex> l(m_);
    auto it = services_.find(s);
    if (it == services_.end()) return BleError::ServiceNotFound;
    if (!it->second.count(c)) return BleError::CharacteristicNotFound;
    writeCbs_[s][c] = std::move(cb);
    return BleError::Success;
}

BleError FakeBleServer::StartAdvertising() {
    std::lock_guard<std::mutex> l(m_);
    if (!initialized_) return BleError::NotInitialized;
    advertising_ = true;
    return BleError::Success;
}
BleError FakeBleServer::StopAdvertising() {
    std::lock_guard<std::mutex> l(m_);
    advertising_ = false;
    return BleError::Success;
}
bool FakeBleServer::IsAdvertising() const { std::lock_guard<std::mutex> l(m_); return advertising_; }

void FakeBleServer::SetConnectionCallback(ConnectionCallback cb) { std::lock_guard<std::mutex> l(m_); connCb_ = std::move(cb); }
void FakeBleServer::SetErrorCallback(ErrorCallback cb) { std::lock_guard<std::mutex> l(m_); errCb_ = std::move(cb); }

std::vector<std::string> FakeBleServer::GetConnectedDevices() const {
    std::lock_guard<std::mutex> l(m_);
    return std::vector<std::string>(devices_.begin(), devices_.end());
}

BleError FakeBleServer::DisconnectDevice(const std::string& id) {
    ConnectionCallback cb;
    {
        std::lock_guard<std::mutex> l(m_);
        if (!initialized_) return BleError::NotInitialized;
        if (!devices_.erase(id)) return BleError::InvalidParameter;
        cb = connCb_;
    }
    if (cb) cb(id, false);
    return BleError::Success;
}

BleError FakeBleServer::UpdateCharacteristicValue(const std::string& s, const std::string& c,
                                                  const std::vector<uint8_t>& v) {
    std::lock_guard<std::mutex> l(m_);
    if (!initialized_) return BleError::NotInitialized;
    auto it = services_.find(s);
    if (it == services_.end()) return BleError::ServiceNotFound;
    auto ct = it->second.find(c);
    if (ct == it->second.end()) return BleError::CharacteristicNotFound;
    ct->second = v;
    valueLog_.push_back(v);
    return BleError::Success;
}

// One transmit attempt (CoreBluetooth updateValue). Requires m_. Same rules as the macOS backend.
NotificationQueue::SendOutcome FakeBleServer::TransmitLocked(const NotificationQueue::Item& item) {
    auto it = services_.find(item.service);
    if (it == services_.end() || !it->second.count(item.characteristic)) return NotificationQueue::SendOutcome::Discard;
    if (credits_ == 0) {
        auto sv = subscribers_.find(item.service);
        const bool anySubscriber = sv != subscribers_.end() && sv->second.count(item.characteristic) &&
                                   !sv->second[item.characteristic].empty();
        return anySubscriber ? NotificationQueue::SendOutcome::Busy : NotificationQueue::SendOutcome::Discard;
    }
    if (credits_ > 0) --credits_;
    notifyLog_.push_back({item.characteristic, item.value});
    ++notifyCount_;
    return NotificationQueue::SendOutcome::Sent;
}

BleError FakeBleServer::NotifyCharacteristicChanged(const std::string& s, const std::string& c, const std::string&) {
    std::vector<std::pair<BleError, std::string>> reports;
    ErrorCallback cb;
    {
        std::lock_guard<std::mutex> l(m_);
        if (!initialized_) return BleError::NotInitialized;
        auto it = services_.find(s);
        if (it == services_.end()) return BleError::ServiceNotFound;
        auto ct = it->second.find(c);
        if (ct == it->second.end()) return BleError::CharacteristicNotFound;
        auto r = queue_.Submit(NotificationQueue::Item{s, c, ct->second},
                               [this](const NotificationQueue::Item& item) { return TransmitLocked(item); });
        if (r.backlogStarted)
            reports.emplace_back(BleError::Success, "Notification queue backed up: transmit queue full on " + c +
                                                        ", queued (depth " + std::to_string(r.depth) + ")");
        if (r.dropped)
            reports.emplace_back(BleError::Unknown, "Notification queue full (" + std::to_string(queue_.Capacity()) +
                                                        "): dropped " + std::to_string(r.dropped) +
                                                        " oldest notification(s)");
        cb = errCb_;
    }
    if (cb) for (auto& [e, msg] : reports) cb(e, msg);
    return BleError::Success;
}

void FakeBleServer::SetTransmitCapacity(int n) { std::lock_guard<std::mutex> l(m_); credits_ = n < 0 ? -1 : n; }

bool FakeBleServer::SetNotifyQueueCapacity(size_t capacity) {
    std::lock_guard<std::mutex> l(m_);
    if (!queue_.Empty()) return false;
    queue_ = NotificationQueue(capacity);
    return true;
}

size_t FakeBleServer::SimulateReadyToUpdate(int credits) {
    std::vector<std::pair<BleError, std::string>> reports;
    ErrorCallback cb;
    ReadyToSendCallback ready;
    size_t sent = 0;
    {
        std::lock_guard<std::mutex> l(m_);
        credits_ = credits < 0 ? -1 : credits;
        if (!initialized_) return 0;
        if (!queue_.Empty()) {
            auto r = queue_.Drain([this](const NotificationQueue::Item& item) { return TransmitLocked(item); });
            sent = r.sent;
            if (r.backlogCleared)
                reports.emplace_back(BleError::Success, "Notification queue drained (sent " + std::to_string(r.sent) +
                                                            " in last batch, high water " +
                                                            std::to_string(queue_.HighWater()) + ")");
        }
        // macOS: one "ready" for every central; legacy values were served first.
        if (gate_.AnyWaiting()) {
            gate_.TakeAllWaiting();
            ready = readyCb_;
        }
        cb = errCb_;
    }
    if (cb) for (auto& [e, msg] : reports) cb(e, msg);
    if (ready) ready(std::string());
    return sent;
}

bool FakeBleServer::IsSubscribedLocked(const std::string& s, const std::string& c, const std::string& dev) const {
    auto sv = subscribers_.find(s);
    if (sv == subscribers_.end()) return false;
    auto ch = sv->second.find(c);
    return ch != sv->second.end() && ch->second.count(dev) > 0;
}

bool FakeBleServer::HasAnySubscriptionLocked(const std::string& dev) const {
    for (const auto& [svc, chars] : subscribers_)
        for (const auto& [chr, devs] : chars)
            if (devs.count(dev)) return true;
    return false;
}

NotifyStatus FakeBleServer::NotifyPeer(const std::string& s, const std::string& c, const std::string& dev,
                                       const uint8_t* data, size_t length) {
    std::lock_guard<std::mutex> l(m_);
    if (!initialized_ || dev.empty() || (!data && length > 0)) return NotifyStatus::Error;
    auto it = services_.find(s);
    if (it == services_.end() || !it->second.count(c)) return NotifyStatus::Error;
    if (!IsSubscribedLocked(s, c, dev)) return NotifyStatus::NotSubscribed;
    auto mp = maxPayload_.find(dev);
    if (mp != maxPayload_.end() && length > mp->second) return NotifyStatus::TooLarge;
    if (failPeerNotifies_ > 0) {
        --failPeerNotifies_;
        return NotifyStatus::Error;
    }
    auto pc = peerCredits_.find(dev);
    const bool peerFull = pc != peerCredits_.end() && pc->second == 0;
    if (peerFull || credits_ == 0 || !queue_.Empty()) {
        gate_.MarkWaiting(dev);
        ++peerBusy_;
        return NotifyStatus::Busy;
    }
    if (pc != peerCredits_.end() && pc->second > 0) --pc->second;
    if (credits_ > 0) --credits_;
    peerLog_.push_back({s, c, dev, std::vector<uint8_t>(data, data + length)});
    ++notifyCount_;
    return NotifyStatus::Sent;
}

size_t FakeBleServer::GetMaxNotifyPayload(const std::string& dev) const {
    std::lock_guard<std::mutex> l(m_);
    auto it = maxPayload_.find(dev);
    return it == maxPayload_.end() ? 0 : it->second;
}

std::vector<std::string> FakeBleServer::GetSubscribers(const std::string& s, const std::string& c) const {
    std::lock_guard<std::mutex> l(m_);
    auto sv = subscribers_.find(s);
    if (sv == subscribers_.end()) return {};
    auto ch = sv->second.find(c);
    if (ch == sv->second.end()) return {};
    return std::vector<std::string>(ch->second.begin(), ch->second.end());
}

void FakeBleServer::SetSubscriptionCallback(SubscriptionCallback cb) { std::lock_guard<std::mutex> l(m_); subCb_ = std::move(cb); }
void FakeBleServer::SetReadyToSendCallback(ReadyToSendCallback cb) { std::lock_guard<std::mutex> l(m_); readyCb_ = std::move(cb); }
void FakeBleServer::SetMaxPayloadCallback(MaxPayloadCallback cb) { std::lock_guard<std::mutex> l(m_); maxPayloadCb_ = std::move(cb); }

uint32_t FakeBleServer::GetCapabilities() const {
    return IBleServer::GetCapabilities() | static_cast<uint32_t>(ServerCapability::PeerNotify);
}

void FakeBleServer::SetPeerTransmitCapacity(const std::string& dev, int n) {
    std::lock_guard<std::mutex> l(m_);
    if (n < 0) peerCredits_.erase(dev); else peerCredits_[dev] = n;
}

bool FakeBleServer::SimulatePeerReady(const std::string& dev, int credits) {
    ReadyToSendCallback cb;
    {
        std::lock_guard<std::mutex> l(m_);
        if (credits < 0) peerCredits_.erase(dev); else peerCredits_[dev] = credits;
        if (!gate_.TakeWaiting(dev)) return false;
        cb = readyCb_;
    }
    if (cb) cb(dev);
    return true;
}

void FakeBleServer::SimulateMtu(const std::string& dev, uint16_t mtu) {
    MaxPayloadCallback cb;
    size_t payload = mtu > 3 ? static_cast<size_t>(mtu) - 3 : 0;
    if (payload == 0) return;
    {
        std::lock_guard<std::mutex> l(m_);
        auto it = maxPayload_.find(dev);
        if (it != maxPayload_.end() && it->second == payload) return;
        maxPayload_[dev] = payload;
        cb = maxPayloadCb_;
    }
    if (cb) cb(dev, payload);
}

void FakeBleServer::SimulateSubscription(const std::string& s, const std::string& c, const std::string& dev,
                                         bool subscribed) {
    ConnectionCallback connCb;
    ErrorCallback errCb;
    SubscriptionCallback subCb;
    std::vector<std::pair<BleError, std::string>> reports;
    bool changed = false;
    {
        std::lock_guard<std::mutex> l(m_);
        auto& subs = subscribers_[s][c];
        if (subscribed) {
            changed = subs.insert(dev).second;
            devices_.insert(dev);
        } else {
            changed = subs.erase(dev) > 0;
            if (subs.empty()) {
                if (size_t dropped = queue_.RemoveCharacteristic(s, c))
                    reports.emplace_back(BleError::Success, "Last subscriber left " + c + ": dropped " +
                                                                std::to_string(dropped) + " queued notification(s)");
            }
            if (!HasAnySubscriptionLocked(dev)) gate_.Forget(dev);
        }
        connCb = connCb_;
        errCb = errCb_;
        subCb = subCb_;
    }
    if (connCb) connCb(dev, subscribed);
    if (subCb && changed) subCb(s, c, dev, subscribed);
    if (errCb) for (auto& [e, msg] : reports) errCb(e, msg);
}

bool FakeBleServer::SimulateWrite(const std::string& s, const std::string& c, const std::string& dev,
                                  const std::vector<uint8_t>& data) {
    return SimulateWriteEx(s, c, dev, data, true);
}

bool FakeBleServer::SimulateWriteEx(const std::string& s, const std::string& c, const std::string& dev,
                                    const std::vector<uint8_t>& data, bool withResponse) {
    CharacteristicWriteCallback cb;
    {
        std::lock_guard<std::mutex> l(m_);
        writeLog_.push_back({dev, data, withResponse});
        auto a = writeCbs_.find(s);
        if (a == writeCbs_.end()) return false;
        auto b = a->second.find(c);
        if (b == a->second.end()) return false;
        cb = b->second;
    }
    if (!cb) return false;
    cb(dev, data);
    return true;
}

LongWrite::Status FakeBleServer::SimulateWriteBatch(const std::string& dev, const std::vector<WriteFragment>& fragments) {
    // Same rules as the macOS backend (src/platforms/macos HandleWrite): one batch = one ATT
    // operation, assembled per characteristic from an empty value, all-or-nothing.
    std::vector<LongWrite::Fragment> frags;
    for (const auto& f : fragments) frags.push_back({f.svc + "/" + f.chr, f.offset, f.data});

    std::vector<std::pair<CharacteristicWriteCallback, std::vector<uint8_t>>> deliveries;
    {
        std::lock_guard<std::mutex> l(m_);
        std::map<std::string, std::pair<std::string, std::string>> keys;
        for (const auto& f : fragments) keys[f.svc + "/" + f.chr] = {f.svc, f.chr};
        auto result = LongWrite::AssembleBatch(frags, [&](const std::string& key, std::vector<uint8_t>& out) {
            out.clear();
            const auto& sc = keys[key];
            auto it = services_.find(sc.first);
            return it != services_.end() && it->second.count(sc.second) != 0;
        });
        if (result.status != LongWrite::Status::Ok) return result.status;
        for (const auto& v : result.values) {
            const auto& sc = keys[v.key];
            services_[sc.first][sc.second] = v.value;
            CharacteristicWriteCallback cb;
            auto a = writeCbs_.find(sc.first);
            if (a != writeCbs_.end()) {
                auto b = a->second.find(sc.second);
                if (b != a->second.end()) cb = b->second;
            }
            if (cb) deliveries.emplace_back(cb, v.value);
        }
    }
    for (auto& d : deliveries) d.first(dev, d.second);
    return LongWrite::Status::Ok;
}

LongWrite::Status FakeBleServer::SimulateWriteAtOffset(const std::string& s, const std::string& c, const std::string& dev,
                                                       size_t offset, const std::vector<uint8_t>& data) {
    // Same rules as the Linux / Windows backends: one fragment applied to the stored value.
    CharacteristicWriteCallback cb;
    std::vector<uint8_t> full;
    {
        std::lock_guard<std::mutex> l(m_);
        auto it = services_.find(s);
        if (it == services_.end() || !it->second.count(c)) return LongWrite::Status::AttributeNotFound;
        auto& value = it->second[c];
        auto status = LongWrite::ApplyFragment(value, offset, data);
        if (status != LongWrite::Status::Ok) return status;
        full = value;
        auto a = writeCbs_.find(s);
        if (a != writeCbs_.end()) {
            auto b = a->second.find(c);
            if (b != a->second.end()) cb = b->second;
        }
    }
    if (cb) cb(dev, full);
    return LongWrite::Status::Ok;
}

bool FakeBleServer::SimulateRead(const std::string& s, const std::string& c, const std::string& dev,
                                 std::vector<uint8_t>& out) {
    CharacteristicReadCallback cb;
    {
        std::lock_guard<std::mutex> l(m_);
        auto a = readCbs_.find(s);
        if (a == readCbs_.end()) return false;
        auto b = a->second.find(c);
        if (b == a->second.end()) return false;
        cb = b->second;
    }
    if (!cb) return false;
    out = cb(dev);
    return true;
}

void FakeBleServer::SimulateConnection(const std::string& dev, bool connected) {
    ConnectionCallback cb;
    SubscriptionCallback subCb;
    std::vector<std::pair<std::string, std::string>> dropped;  // subscriptions ended by the disconnect
    {
        std::lock_guard<std::mutex> l(m_);
        if (connected) {
            devices_.insert(dev);
        } else {
            devices_.erase(dev);
            // Like the OSes: a disconnect ends every subscription of that central.
            for (auto& [svc, chars] : subscribers_)
                for (auto& [chr, devs] : chars)
                    if (devs.erase(dev)) {
                        dropped.emplace_back(svc, chr);
                        if (devs.empty()) queue_.RemoveCharacteristic(svc, chr);
                    }
            gate_.Forget(dev);
            peerCredits_.erase(dev);
            maxPayload_.erase(dev);
        }
        cb = connCb_;
        subCb = subCb_;
    }
    if (subCb) for (auto& [svc, chr] : dropped) subCb(svc, chr, dev, false);
    if (cb) cb(dev, connected);
}

void FakeBleServer::SimulateError(BleError e, const std::string& msg) {
    ErrorCallback cb;
    { std::lock_guard<std::mutex> l(m_); cb = errCb_; }
    if (cb) cb(e, msg);
}

std::vector<uint8_t> FakeBleServer::LastValue(const std::string& s, const std::string& c) const {
    std::lock_guard<std::mutex> l(m_);
    auto it = services_.find(s);
    if (it == services_.end()) return {};
    auto ct = it->second.find(c);
    return ct == it->second.end() ? std::vector<uint8_t>() : ct->second;
}

} // namespace BleServer
