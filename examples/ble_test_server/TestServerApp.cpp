#include "TestServerApp.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <thread>

#include "IBleServer.h"

namespace TestServerApp {

namespace {

uint64_t NowSteadyMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string WallClockStamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    const int ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
    return buf;
}

const char* ErrorName(int code) {
    switch (static_cast<BleServer::BleError>(code)) {
        case BleServer::BleError::Success: return "Success";
        case BleServer::BleError::NotSupported: return "NotSupported";
        case BleServer::BleError::NotInitialized: return "NotInitialized";
        case BleServer::BleError::AlreadyInitialized: return "AlreadyInitialized";
        case BleServer::BleError::InvalidParameter: return "InvalidParameter";
        case BleServer::BleError::ServiceNotFound: return "ServiceNotFound";
        case BleServer::BleError::CharacteristicNotFound: return "CharacteristicNotFound";
        case BleServer::BleError::BluetoothDisabled: return "BluetoothDisabled";
        case BleServer::BleError::AdvertisingFailed: return "AdvertisingFailed";
        case BleServer::BleError::Unknown: return "Unknown";
        case BleServer::BleError::PeerNotReady: return "PeerNotReady";
        case BleServer::BleError::MessageTooLarge: return "MessageTooLarge";
        case BleServer::BleError::BufferTooSmall: return "BufferTooSmall";
    }
    return "?";
}

std::string CodeText(int code) {
    return std::string(ErrorName(code)) + " (" + std::to_string(code) + ")";
}

const char* CharName(const std::string& uuid) {
    if (uuid == TestProfile::kReadUuid) return "read";
    if (uuid == TestProfile::kWriteUuid) return "write";
    if (uuid == TestProfile::kNotifyUuid) return "notify";
    if (uuid == TestProfile::kEchoUuid) return "echo";
    if (uuid == TestProfile::kLargeUuid) return "large";
    return "?";
}

struct CharDef { const char* uuid; uint32_t props; };

}  // namespace

std::string ToHex(const uint8_t* data, size_t n) {
    static const char* d = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        if (i) s += ' ';
        s += d[data[i] >> 4];
        s += d[data[i] & 0xF];
    }
    return s;
}

std::string ToUtf8Text(const uint8_t* data, size_t n) {
    std::string out;
    size_t i = 0;
    while (i < n) {
        const uint8_t b = data[i];
        size_t len = 0;
        if (b >= 0x20 && b < 0x7F) len = 1;
        else if (b >= 0xC2 && b <= 0xDF) len = 2;
        else if (b >= 0xE0 && b <= 0xEF) len = 3;
        else if (b >= 0xF0 && b <= 0xF4) len = 4;
        bool ok = len > 0 && i + len <= n;
        for (size_t k = 1; ok && k < len; ++k) ok = (data[i + k] & 0xC0) == 0x80;
        if (ok) {
            out.append(reinterpret_cast<const char*>(data + i), len);
            i += len;
        } else {
            out += "\xEF\xBF\xBD";  // U+FFFD replacement character
            ++i;
        }
    }
    return out;
}

Bytes MakeLargePayload() {
    Bytes b(TestProfile::kLargeMessageSize);
    for (size_t i = 0; i < b.size(); ++i) b[i] = static_cast<uint8_t>('A' + (i % 26));
    return b;
}

Server::Server(Options options, LogSink sink) : options_(std::move(options)), sink_(std::move(sink)) {}

Server::~Server() { Stop(); }

void Server::Log(const std::string& tag, const std::string& message) {
    std::lock_guard<std::mutex> lock(logMutex_);
    if (sink_) sink_(WallClockStamp() + " [" + tag + "] " + message);
}

void Server::LogVerbose(const std::string& tag, const std::string& message) {
    if (options_.verbose) Log(tag, message);
}

int Server::Check(int code, const char* what) {
    if (code != 0) Log("error", std::string(what) + " failed: " + CodeText(code));
    return code;
}

bool Server::Start() {
    if (started_) return true;
    startMs_ = NowSteadyMs();
    Log("info", "BleServer API " + std::to_string(BleServer_GetApiVersion()) + ", build " +
        (BleServer_GetBuildInfo() ? BleServer_GetBuildInfo() : "?"));

    server_ = BleServer_Create();
    if (!server_) {
        Log("error", "BleServer_Create returned null");
        return false;
    }
    chunker_ = BleChunker_Create();

    // Callbacks first, so nothing that happens during Initialize/advertising is missed.
    BleServer_SetConnectionCallbackEx(server_, &Server::OnConnection, this);
    BleServer_SetErrorCallbackEx(server_, &Server::OnError, this);

    BleServerConfigC config{};
    config.size = sizeof(BleServerConfigC);
    config.deviceName = options_.name.c_str();
    config.requiresPairing = false;
    config.requiresBonding = false;
    config.maxConnections = 5;
    int rc = BleServer_InitializeEx(server_, &config);
    if (rc != 0) {
        const char* last = BleServer_GetLastError(server_);
        Log("error", "Initialize failed: " + CodeText(rc) + (last && *last ? std::string(" - ") + last : ""));
        Stop();
        return false;
    }
    Log("info", "Initialize: Success, name \"" + options_.name + "\"");
    if (options_.verbose) {
        Log("verbose", "capabilities bitmask=" + std::to_string(BleServer_GetCapabilities(server_)));
        BleServer_LogStatus(server_);
    }

    rc = BleServer_AddService(server_, TestProfile::kServiceUuid, true);
    if (Check(rc, "AddService")) { Stop(); return false; }

    const CharDef defs[] = {
        {TestProfile::kReadUuid, TestProfile::kReadProps},
        {TestProfile::kWriteUuid, TestProfile::kWriteProps},
        {TestProfile::kNotifyUuid, TestProfile::kNotifyProps},
        {TestProfile::kEchoUuid, TestProfile::kEchoProps},
        {TestProfile::kLargeUuid, TestProfile::kLargeProps},
    };
    for (const auto& d : defs) {
        const uint32_t perms = ((d.props & (TestProfile::kPropRead)) ? TestProfile::kPermRead : 0u) |
            ((d.props & (TestProfile::kPropWrite | TestProfile::kPropWriteNoResp)) ? TestProfile::kPermWrite : 0u);
        rc = BleServer_AddCharacteristic(server_, TestProfile::kServiceUuid, d.uuid, d.props, perms);
        if (Check(rc, (std::string("AddCharacteristic ") + CharName(d.uuid)).c_str())) { Stop(); return false; }
    }

    // Read characteristics: read (value generated per read) and notify (readable current value).
    Check(BleServer_SetCharacteristicReadCallback(server_, TestProfile::kServiceUuid, TestProfile::kReadUuid,
                                                  &Server::OnRead, this), "SetReadCallback read");
    for (const char* uuid : {TestProfile::kWriteUuid, TestProfile::kEchoUuid, TestProfile::kLargeUuid}) {
        writeContexts_.push_back(std::make_unique<WriteContext>(WriteContext{this, uuid}));
        Check(BleServer_SetCharacteristicWriteCallbackEx(server_, TestProfile::kServiceUuid, uuid,
                                                         &Server::OnWrite, writeContexts_.back().get()),
              (std::string("SetWriteCallback ") + CharName(uuid)).c_str());
    }
    Log("info", "Profile registered: service " + std::string(TestProfile::kServiceUuid) + ", 5 characteristics");

    rc = BleServer_StartAdvertising(server_);
    // CoreBluetooth reports its state asynchronously: right after Initialize it is still "Unknown", and
    // StartAdvertising answers BluetoothDisabled until the OS says PoweredOn. Wait for that (the state
    // changes show up below as [error] lines from the error callback), then give up with the real error.
    const uint64_t waitStart = NowSteadyMs();
    bool announced = false;
    while (rc == static_cast<int>(BleServer::BleError::BluetoothDisabled) &&
           NowSteadyMs() - waitStart < static_cast<uint64_t>(options_.startTimeoutMs)) {
        if (!announced) {
            Log("info", "Bluetooth not ready yet (BluetoothDisabled); retrying for up to " +
                std::to_string(options_.startTimeoutMs / 1000) + " s while the OS reports its state");
            announced = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        rc = BleServer_StartAdvertising(server_);
    }
    if (rc != 0) {
        const char* last = BleServer_GetLastError(server_);
        Log("error", "Start advertising failed: " + CodeText(rc) + (last && *last ? std::string(" - ") + last : ""));
        if (rc == static_cast<int>(BleServer::BleError::BluetoothDisabled)) {
            Log("error", "Bluetooth never became ready. Check that Bluetooth is on and that this program (or the "
                "terminal running it) is allowed to use it; see docs/TESTING.md, per-OS setup.");
        }
        Stop();
        return false;
    }
    started_ = true;
    Log("info", "Advertising as \"" + options_.name + "\"");
    return true;
}

void Server::Stop() {
    if (!server_ && !chunker_) return;
    if (server_) {
        if (BleServer_IsAdvertising(server_)) {
            const int rc = BleServer_StopAdvertising(server_);
            Log("info", "Stop advertising: " + CodeText(rc));
        }
        BleServer_Shutdown(server_);
    }
    if (chunker_) BleChunker_Destroy(chunker_);
    if (server_) BleServer_Destroy(server_);  // drains running callbacks; `this` stays valid until here
    chunker_ = nullptr;
    server_ = nullptr;
    started_ = false;
    Log("info", "Server shut down");
}

bool Server::IsAdvertising() const { return server_ && BleServer_IsAdvertising(server_); }
int Server::ConnectedCount() const { return server_ ? BleServer_GetConnectedDeviceCount(server_) : 0; }

int Server::Notify(const char* chr, const Bytes& value, const char* what) {
    if (!server_) return static_cast<int>(BleServer::BleError::NotInitialized);
    int rc = BleServer_UpdateCharacteristicValue(server_, TestProfile::kServiceUuid, chr, value.data(),
                                                 static_cast<int>(value.size()));
    if (rc != 0) return Check(rc, (std::string("UpdateCharacteristicValue ") + what).c_str());
    rc = BleServer_NotifyCharacteristicChanged(server_, TestProfile::kServiceUuid, chr, "");
    if (rc != 0) return Check(rc, (std::string("Notify ") + what).c_str());
    return 0;
}

int Server::SendHeartbeat() {
    if (!server_ || ConnectedCount() == 0) return 0;
    uint32_t n;
    { std::lock_guard<std::mutex> l(stateMutex_); n = ++heartbeatCount_; }
    const std::string text = "heartbeat #" + std::to_string(n);
    const int rc = Notify(TestProfile::kNotifyUuid, Bytes(text.begin(), text.end()), "heartbeat");
    if (rc == 0 && (options_.verbose || n % 10 == 1)) {
        Log("notify", "sent \"" + text + "\"" + (options_.verbose ? "" : " (every 10th heartbeat is logged; --verbose logs all)"));
    }
    return rc;
}

int Server::SendManualNotify() {
    uint32_t n;
    { std::lock_guard<std::mutex> l(stateMutex_); n = ++manualCount_; }
    const std::string text = "manual #" + std::to_string(n);
    const int rc = Notify(TestProfile::kNotifyUuid, Bytes(text.begin(), text.end()), "manual notify");
    if (rc == 0) Log("notify", "sent \"" + text + "\" (connected devices: " + std::to_string(ConnectedCount()) + ")");
    return rc;
}

int Server::SendLargeMessage() {
    if (!server_ || !chunker_) return static_cast<int>(BleServer::BleError::NotInitialized);
    const Bytes payload = MakeLargePayload();
    const uint32_t chunks = static_cast<uint32_t>(
        (payload.size() + BleServer::BleMessageChunker::MAX_PAYLOAD_SIZE - 1) / BleServer::BleMessageChunker::MAX_PAYLOAD_SIZE);
    Log("large", "sending " + std::to_string(payload.size()) + " bytes as " + std::to_string(chunks) +
        " chunks of up to " + std::to_string(BleServer::BleMessageChunker::CHUNK_SIZE) +
        " bytes (needs ATT MTU >= 259 on the central)");
    const int rc = BleChunker_SendMessage(chunker_, server_, TestProfile::kServiceUuid, TestProfile::kLargeUuid,
                                          payload.data(), static_cast<int>(payload.size()));
    if (rc != 0) return Check(rc, "SendLargeMessage");
    { std::lock_guard<std::mutex> l(stateMutex_); lastLargeChunks_ = chunks; }
    Log("large", "all " + std::to_string(chunks) + " chunks notified; waiting for ACKs (written back to the large characteristic)");
    return 0;
}

int Server::RetransmitTick() {
    if (!chunker_) return 0;
    const int sent = BleChunker_CleanupTimedOut(chunker_, TestProfile::kChunkAckTimeoutMs);
    if (sent < 0) return Check(sent, "RetransmitTick");
    if (sent > 0) Log("large", "retransmitted " + std::to_string(sent) + " unacknowledged chunk(s)");
    return 0;
}

int Server::ToggleAdvertising() {
    if (!server_) return static_cast<int>(BleServer::BleError::NotInitialized);
    if (BleServer_IsAdvertising(server_)) {
        const int rc = BleServer_StopAdvertising(server_);
        Log("info", "Stop advertising: " + CodeText(rc));
        return rc;
    }
    const int rc = BleServer_StartAdvertising(server_);
    Log("info", "Start advertising: " + CodeText(rc));
    return rc;
}

// ---- native callbacks (arrive on backend threads) ---------------------------------------------

int Server::OnRead(const char* deviceId, uint8_t* buffer, int bufferLength, void* userData) {
    auto* self = static_cast<Server*>(userData);
    const std::string device = deviceId ? deviceId : "";
    // The C layer calls twice per read: once with a null buffer to learn the size, once to fill it.
    // Build the value on the first call only so one central read counts as one read.
    if (!buffer) {
        std::lock_guard<std::mutex> l(self->stateMutex_);
        const uint32_t n = ++self->readCount_;
        char text[64];
        std::snprintf(text, sizeof(text), "uptime=%.1fs read#%u",
                      static_cast<double>(NowSteadyMs() - self->startMs_) / 1000.0, n);
        Bytes v(text, text + std::char_traits<char>::length(text));
        self->readValues_[device] = v;
        return static_cast<int>(v.size());
    }
    Bytes v;
    {
        std::lock_guard<std::mutex> l(self->stateMutex_);
        auto it = self->readValues_.find(device);
        if (it != self->readValues_.end()) { v = std::move(it->second); self->readValues_.erase(it); }
    }
    const int n = std::min(bufferLength, static_cast<int>(v.size()));
    if (n > 0) std::copy(v.begin(), v.begin() + n, buffer);
    self->Log("read", "served to " + device + ": \"" + std::string(v.begin(), v.end()) + "\"");
    return n;
}

void Server::OnWrite(const char* deviceId, const uint8_t* data, int dataLength, void* userData) {
    // The write callback does not say which characteristic was written, so every characteristic is
    // registered with its own context (userData) carrying its UUID.
    auto* ctx = static_cast<WriteContext*>(userData);
    const Bytes bytes(data, data + (dataLength > 0 ? dataLength : 0));
    ctx->self->HandleWrite(ctx->uuid, deviceId ? deviceId : "", bytes);
}

void Server::HandleWrite(const std::string& chr, const std::string& device, const Bytes& data) {
    const std::string desc = "write on " + std::string(CharName(chr)) + " from " + device + ": " +
        std::to_string(data.size()) + " B";
    if (chr == TestProfile::kLargeUuid) {
        HandleLargeWrite(device, data);
        return;
    }
    Log("write", std::string(desc) + " hex=" + ToHex(data.data(), data.size()) +
        " text=\"" + ToUtf8Text(data.data(), data.size()) + "\"");
    if (chr == TestProfile::kEchoUuid) {
        if (data.empty()) { Log("echo", "empty write, nothing to echo"); return; }
        if (Notify(TestProfile::kEchoUuid, data, "echo") == 0) {
            Log("notify", "echo -> notify back " + std::to_string(data.size()) + " B");
        }
    }
}

void Server::HandleLargeWrite(const std::string& device, const Bytes& data) {
    using BleServer::BleMessageChunker;
    if (!chunker_) return;
    Log("write", "write on large from " + device + ": " + std::to_string(data.size()) + " B hex=" +
        ToHex(data.data(), std::min<size_t>(data.size(), 16)) + (data.size() > 16 ? " ..." : ""));
    if (!BleMessageChunker::IsValidChunk(data)) {
        Log("large", "not a valid chunk or ACK packet (header: type 01/02, msg id, index, total); ignored");
        return;
    }
    const uint32_t id = BleMessageChunker::GetMessageId(data);
    const uint32_t index = BleMessageChunker::GetChunkIndex(data);
    uint8_t ack[BleMessageChunker::HEADER_SIZE];
    int ackLen = static_cast<int>(sizeof(ack));
    const int rc = BleChunker_ProcessIncoming(chunker_, data.data(), static_cast<int>(data.size()), ack, &ackLen);
    if (rc < 0 || rc > 1) { Check(rc, "ProcessIncoming"); return; }
    if (BleMessageChunker::IsAck(data)) {
        size_t acked, total;
        {
            std::lock_guard<std::mutex> l(stateMutex_);
            auto& s = ackedChunks_[id];
            s.insert(index);
            acked = s.size();
            total = lastLargeChunks_;
        }
        Log("large", "ACK for message " + std::to_string(id) + " chunk " + std::to_string(index) +
            " (" + std::to_string(acked) + "/" + std::to_string(total) + " chunks acknowledged)");
        return;
    }
    const uint32_t total = BleMessageChunker::GetTotalChunks(data);
    Log("large", "received chunk " + std::to_string(index + 1) + "/" + std::to_string(total) + " of message " +
        std::to_string(id));
    if (rc == 1) {
        const int size = BleChunker_GetNextMessageSize(chunker_);
        Bytes msg(static_cast<size_t>(size > 0 ? size : 0));
        int len = size;
        if (size >= 0 && BleChunker_ReadNextMessage(chunker_, msg.data(), &len)) {
            msg.resize(static_cast<size_t>(len));
            Log("large", "message " + std::to_string(id) + " reassembled: " + std::to_string(msg.size()) +
                " B, text=\"" + ToUtf8Text(msg.data(), std::min<size_t>(msg.size(), 64)) + "\"");
        }
    }
}

void Server::OnConnection(const char* deviceId, bool connected, void* userData) {
    auto* self = static_cast<Server*>(userData);
    const std::string device = deviceId ? deviceId : "";
    if (!connected) {
        std::lock_guard<std::mutex> l(self->stateMutex_);
        self->readValues_.erase(device);
    }
    // macOS (CoreBluetooth) reports a central only once it subscribes; Linux/Windows report the connection.
    self->Log(connected ? "connect" : "disconnect",
              std::string("central ") + device +
              (connected ? " connected (macOS: this means it subscribed)" : " disconnected (macOS: unsubscribed)"));
}

void Server::OnError(int error, const char* message, void* userData) {
    auto* self = static_cast<Server*>(userData);
    self->Log("error", "BleError " + CodeText(error) + ": " + (message ? message : ""));
}

}  // namespace TestServerApp
