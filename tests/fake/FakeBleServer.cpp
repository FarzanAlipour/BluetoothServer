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
    services_.clear();
    writeCbs_.clear();
    readCbs_.clear();
    connCb_ = nullptr;
    errCb_ = nullptr;
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

BleError FakeBleServer::NotifyCharacteristicChanged(const std::string& s, const std::string& c, const std::string&) {
    std::lock_guard<std::mutex> l(m_);
    if (!initialized_) return BleError::NotInitialized;
    auto it = services_.find(s);
    if (it == services_.end()) return BleError::ServiceNotFound;
    if (!it->second.count(c)) return BleError::CharacteristicNotFound;
    ++notifyCount_;
    return BleError::Success;
}

bool FakeBleServer::SimulateWrite(const std::string& s, const std::string& c, const std::string& dev,
                                  const std::vector<uint8_t>& data) {
    CharacteristicWriteCallback cb;
    {
        std::lock_guard<std::mutex> l(m_);
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

void FakeBleServer::SimulateConnection(const std::string& dev, bool connected) {
    ConnectionCallback cb;
    {
        std::lock_guard<std::mutex> l(m_);
        if (connected) devices_.insert(dev); else devices_.erase(dev);
        cb = connCb_;
    }
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
