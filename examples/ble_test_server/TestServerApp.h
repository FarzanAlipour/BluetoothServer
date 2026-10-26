#pragma once
// The GATT test server logic, written against the C ABI only (the same surface Unity uses).
// ble_test_server (real backend) and tests/test_scenario_gatt.cpp (fake backend) both compile this.
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "TestProfile.h"

namespace TestServerApp {

using Bytes = std::vector<uint8_t>;

struct Options {
    std::string name = TestProfile::kDefaultName;
    bool verbose = false;
    // How long Start() keeps retrying StartAdvertising while the OS still reports Bluetooth as not ready.
    int startTimeoutMs = 10000;
};

// Receives one finished log line (timestamp already included). Called from any thread; calls are serialized.
using LogSink = std::function<void(const std::string& line)>;

std::string ToHex(const uint8_t* data, size_t n);
// Printable UTF-8 view of bytes: invalid sequences and control bytes become U+FFFD / \xNN.
std::string ToUtf8Text(const uint8_t* data, size_t n);
// Deterministic payload of the large-message action: byte i = 'A' + (i % 26).
Bytes MakeLargePayload();

class Server {
public:
    Server(Options options, LogSink sink);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Create, initialize, register the profile and callbacks, start advertising. False on failure
    // (the reason is logged, including the BleError code). Safe to call Stop() afterwards.
    bool Start();
    // Stop advertising, shut down and destroy. Idempotent.
    void Stop();

    // Actions (stdin commands and tests). Each returns the BleError code (0 = Success).
    int SendHeartbeat();          // one heartbeat notify; no-op (returns 0) while nobody is connected
    int SendManualNotify();       // "manual #N" notify now
    int SendLargeMessage();       // kLargeMessageSize bytes as chunks on the large characteristic
    int ToggleAdvertising();      // stop if advertising, start otherwise
    int RetransmitTick();         // resend unacknowledged chunks older than kChunkAckTimeoutMs

    bool IsAdvertising() const;
    int ConnectedCount() const;
    void* Handle() const { return server_; }
    std::string Name() const { return options_.name; }

private:
    struct WriteContext { Server* self; std::string uuid; };

    static int OnRead(const char* deviceId, uint8_t* buffer, int bufferLength, void* userData);
    static void OnWrite(const char* deviceId, const uint8_t* data, int dataLength, void* userData);
    static void OnConnection(const char* deviceId, bool connected, void* userData);
    static void OnError(int error, const char* message, void* userData);

    void HandleWrite(const std::string& chr, const std::string& device, const Bytes& data);
    void HandleLargeWrite(const std::string& device, const Bytes& data);
    int Notify(const char* chr, const Bytes& value, const char* what);
    int Check(int code, const char* what);
    void Log(const std::string& tag, const std::string& message);
    void LogVerbose(const std::string& tag, const std::string& message);

    Options options_;
    LogSink sink_;
    std::mutex logMutex_;
    std::mutex stateMutex_;
    void* server_ = nullptr;
    void* chunker_ = nullptr;
    bool started_ = false;
    uint64_t startMs_ = 0;
    uint32_t readCount_ = 0;
    uint32_t heartbeatCount_ = 0;
    uint32_t manualCount_ = 0;
    uint32_t lastLargeChunks_ = 0;
    std::map<std::string, Bytes> readValues_;                 // read-callback size query -> fill call
    std::vector<std::unique_ptr<WriteContext>> writeContexts_;  // stable userData for write callbacks
    std::map<uint32_t, std::set<uint32_t>> ackedChunks_;     // message id -> chunk indexes acknowledged
};

}  // namespace TestServerApp
