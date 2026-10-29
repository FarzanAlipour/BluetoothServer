#pragma once
// In-memory IBleServer used by tests and by -DBLE_BACKEND=fake. No Bluetooth, no OS APIs.
#include "IBleServer.h"
#include "NotificationQueue.h"
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
    // Asks the registered read callback for the value a central would read. Returns false if none.
    bool SimulateRead(const std::string& svc, const std::string& chr, const std::string& device,
                      std::vector<uint8_t>& out);
    void SimulateConnection(const std::string& device, bool connected);
    void SimulateError(BleError e, const std::string& message);
    std::vector<uint8_t> LastValue(const std::string& svc, const std::string& chr) const;
    int NotifyCount() const { return notifyCount_.load(); }
    // Every value written through UpdateCharacteristicValue, in order (what a central would see).
    std::vector<std::vector<uint8_t>> ValueLog() const { std::lock_guard<std::mutex> l(m_); return valueLog_; }

    // Every notification sent, in order: which characteristic and the value it carried when notified
    // (what a subscribed central would receive). Added for the GATT scenario test.
    struct NotifyEvent { std::string characteristic; std::vector<uint8_t> value; };
    std::vector<NotifyEvent> NotifyLog() const { std::lock_guard<std::mutex> l(m_); return notifyLog_; }

    // When true, Shutdown() keeps the registered backend callbacks, so tests can prove that
    // src/common gates delivery itself (a real backend may still emit a late event).
    void SetRetainCallbacksOnShutdown(bool v) { std::lock_guard<std::mutex> l(m_); retain_ = v; }

    // --- Notification flow control (models the macOS backend; same NotificationQueue) ---
    // How many notifications the simulated OS transmit queue accepts before it reports "full"
    // (CoreBluetooth: updateValue returns NO). -1 = unlimited (default, no flow control).
    void SetTransmitCapacity(int notifications);
    // Resizes the pending-notification bound (default NotificationQueue::kDefaultCapacity).
    // Only allowed while nothing is queued; returns false otherwise.
    bool SetNotifyQueueCapacity(size_t capacity);
    // The OS has room again (CoreBluetooth: peripheralManagerIsReadyToUpdateSubscribers:): sets the
    // transmit credit to `credits` (-1 = unlimited) and drains the queue in order. Returns the number
    // sent. Reports through the error callback WITHOUT any fake-internal lock held.
    size_t SimulateReadyToUpdate(int credits = -1);
    // A central (un)subscribes to a characteristic. Mirrors macOS: fires the connection callback, and
    // when the last subscriber leaves, that characteristic's queued notifications are dropped.
    void SimulateSubscription(const std::string& svc, const std::string& chr, const std::string& device,
                              bool subscribed);
    size_t PendingNotifications() const { std::lock_guard<std::mutex> l(m_); return queue_.Size(); }

    // Most recently created instance still alive (nullptr otherwise).
    static FakeBleServer* Last();

private:
    mutable std::mutex m_;
    bool initialized_ = false;
    bool advertising_ = false;
    bool retain_ = false;
    std::map<std::string, std::map<std::string, std::vector<uint8_t>>> services_;
    std::map<std::string, std::map<std::string, CharacteristicWriteCallback>> writeCbs_;
    std::map<std::string, std::map<std::string, CharacteristicReadCallback>> readCbs_;
    std::set<std::string> devices_;
    ConnectionCallback connCb_;
    ErrorCallback errCb_;
    std::atomic<int> notifyCount_{0};
    std::vector<std::vector<uint8_t>> valueLog_;
    std::vector<NotifyEvent> notifyLog_;

    // Flow control state (guarded by m_).
    NotificationQueue queue_;
    int credits_ = -1;  // -1 unlimited
    std::map<std::string, std::map<std::string, std::set<std::string>>> subscribers_;
    NotificationQueue::SendOutcome TransmitLocked(const NotificationQueue::Item& item);
};

} // namespace BleServer
