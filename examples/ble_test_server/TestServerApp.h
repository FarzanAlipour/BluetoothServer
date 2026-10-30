#pragma once
// The GATT test server logic, written against the C ABI only (the same surface Unity uses).
// ble_test_server (real backend) and tests/test_scenario_gatt.cpp (fake backend) both compile this.
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "TestProfile.h"

namespace TestServerApp {

using Bytes = std::vector<uint8_t>;

struct Options {
    std::string name = TestProfile::kDefaultName;
    bool verbose = false;
    // Send every received BleLink message back to its sender as a new message (--echo-messages).
    bool echoMessages = false;
    // How long Start() keeps retrying StartAdvertising while the OS still reports Bluetooth as not ready.
    int startTimeoutMs = 10000;
};

// Receives one finished log line (timestamp already included). Called from any thread; calls are serialized.
using LogSink = std::function<void(const std::string& line)>;

std::string ToHex(const uint8_t* data, size_t n);
// Printable UTF-8 view of bytes: invalid sequences and control bytes become U+FFFD.
std::string ToUtf8Text(const uint8_t* data, size_t n);
// Deterministic blob of the `b` command: byte i = (i * 7 + 3) mod 256, the payloadRule of
// docs/protocol-test-vectors.json (the Python reference client uses the same rule).
Bytes MakeBlob(size_t n);
// "1000", "64k", "64K", "1M", "1.5m" -> bytes (k = 1024). False for junk or > kMaxBlobSize.
bool ParseSize(const std::string& text, size_t* out);
// CRC-32/IEEE (zlib), as in docs/PROTOCOL.md 2.4. Logged so a client can compare.
uint32_t Crc32(const uint8_t* data, size_t n);

class Server {
public:
    Server(Options options, LogSink sink);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Create, initialize, register the test profile, the BleLink service and callbacks, start
    // advertising. False on failure (the reason is logged, including the BleError code). Safe to call
    // Stop() afterwards.
    bool Start();
    // Stop advertising, shut down and destroy. Idempotent.
    void Stop();

    // Actions (stdin commands and tests). Each returns the BleError code (0 = Success).
    int SendHeartbeat();          // one heartbeat notify; no-op (returns 0) while nobody is connected
    int SendManualNotify();       // "manual #N" notify now
    int ToggleAdvertising();      // stop if advertising, start otherwise
    // BleLink: one message to every READY peer. PeerNotReady (10) when there is none; otherwise the
    // first per-peer error, or 0 when every peer accepted it.
    int SendText(const std::string& text);  // `m <text>`
    int SendBlob(size_t size);              // `b <size>`: MakeBlob(size)
    // `p`: logs every BleLink peer (packet size, MTU, in-flight messages). Returns the peer count.
    int ListPeers();
    // Takes every pending BleLink event and logs/handles it (echo mode). Call often (main loop).
    // Returns the number of events handled.
    int PumpLink();

    bool IsAdvertising() const;
    int ConnectedCount() const;
    int ReadyPeerCount() const;
    void* Handle() const { return server_; }
    void* Link() const { return link_; }
    std::string Name() const { return options_.name; }

private:
    struct WriteContext { Server* self; std::string uuid; };
    struct Outgoing {
        std::string device; size_t size = 0; uint64_t startMs = 0; bool echo = false;
        uint64_t lastSending = 0, lastPeerReceived = 0;  // last PROGRESS bytes seen (log throttling)
    };
    struct IncomingStart { uint64_t ms = 0; uint64_t bytes = 0; uint64_t last = 0; };

    static int OnRead(const char* deviceId, uint8_t* buffer, int bufferLength, void* userData);
    static void OnWrite(const char* deviceId, const uint8_t* data, int dataLength, void* userData);
    static void OnConnection(const char* deviceId, bool connected, void* userData);
    static void OnError(int error, const char* message, void* userData);

    void HandleWrite(const std::string& chr, const std::string& device, const Bytes& data);
    int Notify(const char* chr, const Bytes& value, const char* what);
    int SendToReadyPeers(const Bytes& data, const std::string& what);
    int SendTo(const std::string& device, const Bytes& data, bool echo);
    std::vector<std::string> PeerIds() const;
    int Check(int code, const char* what);
    void Log(const std::string& tag, const std::string& message);
    void LogVerbose(const std::string& tag, const std::string& message);

    Options options_;
    LogSink sink_;
    std::mutex logMutex_;
    std::mutex stateMutex_;
    void* server_ = nullptr;
    void* link_ = nullptr;
    bool started_ = false;
    uint64_t startMs_ = 0;
    uint32_t readCount_ = 0;
    uint32_t heartbeatCount_ = 0;
    uint32_t manualCount_ = 0;
    std::map<std::string, Bytes> readValues_;                 // read-callback size query -> fill call
    std::vector<std::unique_ptr<WriteContext>> writeContexts_;  // stable userData for write callbacks
    // BleLink bookkeeping (PumpLink and the send commands run on the same thread).
    std::map<uint32_t, Outgoing> outgoing_;                               // BleLink_Send id -> info
    std::map<std::pair<std::string, uint32_t>, IncomingStart> incoming_;  // (device, wire id) -> first PROGRESS
};

}  // namespace TestServerApp
