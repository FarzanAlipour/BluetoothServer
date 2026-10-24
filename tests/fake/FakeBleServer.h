#pragma once
// In-memory IBleServer used by tests and by -DBLE_BACKEND=fake. No Bluetooth, no OS APIs.
#include "IBleServer.h"
#include <atomic>
#include <map>
#include <mutex>
#include <set>

namespace BleServer {

class FakeBleServer : public IBleServer {
public:
    FakeBleServer();
    ~FakeBleServer() override;

    // --- IBleServer ---
    BleError Initialize(const ServerConfig& config) override;
    void Shutdown() override;
    bool IsInitialized() const override;
    BleError AddService(const ServiceConfig& config) override;
    BleError RemoveService(const std::string& serviceUuid) override;
    std::vector<std::string> GetServices() const override;
    BleError AddCharacteristic(const std::string& serviceUuid, const CharacteristicConfig& config) override;
    BleError RemoveCharacteristic(const std::string& serviceUuid, const std::string& characteristicUuid) override;
    BleError SetCharacteristicReadCallback(const std::string& serviceUuid, const std::string& characteristicUuid,
                                           CharacteristicReadCallback callback) override;
    BleError SetCharacteristicWriteCallback(const std::string& serviceUuid, const std::string& characteristicUuid,
                                            CharacteristicWriteCallback callback) override;
    BleError StartAdvertising() override;
    BleError StopAdvertising() override;
    bool IsAdvertising() const override;
    void SetConnectionCallback(ConnectionCallback callback) override;
    std::vector<std::string> GetConnectedDevices() const override;
    BleError DisconnectDevice(const std::string& deviceId) override;
    BleError UpdateCharacteristicValue(const std::string& serviceUuid, const std::string& characteristicUuid,
                                       const std::vector<uint8_t>& value) override;
    BleError NotifyCharacteristicChanged(const std::string& serviceUuid, const std::string& characteristicUuid,
                                         const std::string& deviceId = "") override;
    void SetErrorCallback(ErrorCallback callback) override;

    // --- Test hooks (the "remote side") ---
    // Delivers a central's write to the registered callback. Returns false if no callback.
    // The callback is invoked WITHOUT any fake-internal lock held.
    bool SimulateWrite(const std::string& svc, const std::string& chr, const std::string& device,
                       const std::vector<uint8_t>& data);
    void SimulateConnection(const std::string& device, bool connected);
    void SimulateError(BleError e, const std::string& message);
    std::vector<uint8_t> LastValue(const std::string& svc, const std::string& chr) const;
    int NotifyCount() const { return notifyCount_.load(); }
    // Every value written through UpdateCharacteristicValue, in order (what a central would see).
    std::vector<std::vector<uint8_t>> ValueLog() const { std::lock_guard<std::mutex> l(m_); return valueLog_; }

    // Most recently created instance still alive (nullptr otherwise).
    static FakeBleServer* Last();

private:
    mutable std::mutex m_;
    bool initialized_ = false;
    bool advertising_ = false;
    std::map<std::string, std::map<std::string, std::vector<uint8_t>>> services_;
    std::map<std::string, std::map<std::string, CharacteristicWriteCallback>> writeCbs_;
    std::map<std::string, std::map<std::string, CharacteristicReadCallback>> readCbs_;
    std::set<std::string> devices_;
    ConnectionCallback connCb_;
    ErrorCallback errCb_;
    std::atomic<int> notifyCount_{0};
    std::vector<std::vector<uint8_t>> valueLog_;
};

} // namespace BleServer
