#include "TestServerApp.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iterator>
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

const char* ReasonName(int reason) {
    switch (reason) {
        case BLELINK_REASON_NONE: return "none";
        case BLELINK_REASON_DISCONNECTED: return "disconnected";
        case BLELINK_REASON_RESET: return "reset";
        case BLELINK_REASON_TIMEOUT: return "timeout";
        case BLELINK_REASON_CRC: return "crc";
        case BLELINK_REASON_TOO_LARGE: return "too_large";
        case BLELINK_REASON_BUSY: return "busy";
        case BLELINK_REASON_REJECTED: return "rejected";
        case BLELINK_REASON_LINK_UNRELIABLE: return "link_unreliable";
        case BLELINK_REASON_CANCELLED: return "cancelled";
        case BLELINK_REASON_NOT_READY: return "not_ready";
        case BLELINK_REASON_SHUTDOWN: return "shutdown";
    }
    return "?";
}

const char* CharName(const std::string& uuid) {
    if (uuid == TestProfile::kReadUuid) return "read";
    if (uuid == TestProfile::kWriteUuid) return "write";
    if (uuid == TestProfile::kNotifyUuid) return "notify";
    if (uuid == TestProfile::kEchoUuid) return "echo";
    return "?";
}

// "1.5 KB/s"; "n/a" when the time is too short to measure.
std::string Rate(uint64_t bytes, uint64_t ms) {
    if (ms == 0) return "n/a";
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.1f KB/s", static_cast<double>(bytes) / 1024.0 / (static_cast<double>(ms) / 1000.0));
    return buf;
}

std::string Seconds(uint64_t ms) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f s", static_cast<double>(ms) / 1000.0);
    return buf;
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

Bytes MakeBlob(size_t n) {
    Bytes b(n);
    for (size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
    return b;
}

bool ParseSize(const std::string& text, size_t* out) {
    std::string t = text;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r')) t.pop_back();
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
    if (t.empty() || !out) return false;
    double mult = 1;
    const char last = t.back();
    if (last == 'k' || last == 'K') { mult = 1024; t.pop_back(); }
    else if (last == 'm' || last == 'M') { mult = 1024.0 * 1024.0; t.pop_back(); }
    if (t.empty()) return false;
    for (char c : t) if (!((c >= '0' && c <= '9') || c == '.')) return false;
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (!end || *end != '\0') return false;
    const double bytes = v * mult;
    if (bytes < 0 || bytes > static_cast<double>(TestProfile::kMaxBlobSize)) return false;
    *out = static_cast<size_t>(bytes);
    return true;
}

uint32_t Crc32(const uint8_t* data, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= data[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
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
    for (const char* uuid : {TestProfile::kWriteUuid, TestProfile::kEchoUuid}) {
        writeContexts_.push_back(std::make_unique<WriteContext>(WriteContext{this, uuid}));
        Check(BleServer_SetCharacteristicWriteCallbackEx(server_, TestProfile::kServiceUuid, uuid,
                                                         &Server::OnWrite, writeContexts_.back().get()),
              (std::string("SetWriteCallback ") + CharName(uuid)).c_str());
    }
    Log("info", "Profile registered: service " + std::string(TestProfile::kServiceUuid) + ", 4 characteristics");

    // BleLink (messages of any size, docs/PROTOCOL.md): must exist before advertising starts.
    BleLinkConfigC linkConfig{};
    linkConfig.size = sizeof(linkConfig);  // all limits 0 = defaults
    link_ = BleLink_Create(server_, &linkConfig);
    if (!link_) {
        const char* last = BleServer_GetLastError(server_);
        Log("error", std::string("BleLink_Create failed") + (last && *last ? std::string(": ") + last : ""));
        Stop();
        return false;
    }
    Log("info", std::string("BleLink service ") + BLELINK_SERVICE_UUID +
        " registered (RX ...1001, TX ...1002, INFO ...1003); we accept messages up to 16 MiB, window 4, "
        "packets up to 512 B" + (options_.echoMessages ? "; echo mode ON (every message is sent back)" : ""));

    rc = BleServer_StartAdvertising(server_);
    // CoreBluetooth reports its state asynchronously: right after Initialize it is still "Unknown", and
    // StartAdvertising answers BluetoothDisabled until the OS says PoweredOn. Wait for that (the state
    // changes show up below as [backend] lines from the error callback), then give up with the real error.
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
    if (!server_ && !link_) return;
    if (server_) {
        if (BleServer_IsAdvertising(server_)) {
            const int rc = BleServer_StopAdvertising(server_);
            Log("info", "Stop advertising: " + CodeText(rc));
        }
        // Shuts the link down first: unfinished messages fail with reason shutdown (logged below).
        BleServer_Shutdown(server_);
    }
    if (link_) {
        PumpLink();
        BleLink_Destroy(link_);
    }
    if (server_) BleServer_Destroy(server_);  // drains running callbacks; `this` stays valid until here
    link_ = nullptr;
    server_ = nullptr;
    started_ = false;
    outgoing_.clear();
    incoming_.clear();
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

// ---- BleLink ------------------------------------------------------------------------------------

std::vector<std::string> Server::PeerIds() const {
    std::vector<std::string> ids;
    if (!link_) return ids;
    const int n = BleLink_GetPeerCount(link_);
    for (int i = 0; i < n; ++i) {
        char buf[BLELINK_DEVICE_ID_CAPACITY];
        int len = static_cast<int>(sizeof(buf));
        if (BleLink_GetPeerId(link_, i, buf, &len) == 0) ids.emplace_back(buf);
    }
    return ids;
}

int Server::ReadyPeerCount() const {
    int ready = 0;
    for (const auto& id : PeerIds()) {
        BleLinkPeerInfoC info{};
        info.size = sizeof(info);
        if (BleLink_GetPeerInfo(link_, id.c_str(), &info) == 0 && info.ready) ++ready;
    }
    return ready;
}

int Server::SendTo(const std::string& device, const Bytes& data, bool echo) {
    uint32_t id = 0;
    const int rc = BleLink_Send(link_, device.c_str(), data.empty() ? nullptr : data.data(),
                                static_cast<int>(data.size()), &id);
    if (rc != 0) {
        Log("error", std::string(echo ? "echo to " : "send to ") + device + " (" + std::to_string(data.size()) +
            " B) failed: " + CodeText(rc));
        return rc;
    }
    outgoing_[id] = Outgoing{device, data.size(), NowSteadyMs(), echo};
    char crc[16];
    std::snprintf(crc, sizeof(crc), "%08X", Crc32(data.data(), data.size()));
    Log(echo ? "echo" : "send", "message " + std::to_string(id) + " to " + device + ": " +
        std::to_string(data.size()) + " B queued, crc32=" + crc);
    return 0;
}

int Server::SendToReadyPeers(const Bytes& data, const std::string& what) {
    if (!link_) return static_cast<int>(BleServer::BleError::NotInitialized);
    int first = 0, sent = 0, peers = 0;
    for (const auto& id : PeerIds()) {
        BleLinkPeerInfoC info{};
        info.size = sizeof(info);
        if (BleLink_GetPeerInfo(link_, id.c_str(), &info) != 0 || !info.ready) continue;
        ++peers;
        const int rc = SendTo(id, data, false);
        if (rc == 0) ++sent;
        else if (first == 0) first = rc;
    }
    if (peers == 0) {
        Log("send", what + ": no READY BleLink peer (a central must subscribe to TX and write HELLO; `p` lists peers)");
        return static_cast<int>(BleServer::BleError::PeerNotReady);
    }
    Log("send", what + ": queued for " + std::to_string(sent) + "/" + std::to_string(peers) + " ready peer(s)");
    return first;
}

int Server::SendText(const std::string& text) {
    return SendToReadyPeers(Bytes(text.begin(), text.end()), "text \"" + text + "\" (" + std::to_string(text.size()) + " B)");
}

int Server::SendBlob(size_t size) {
    if (size > TestProfile::kMaxBlobSize) return Check(static_cast<int>(BleServer::BleError::InvalidParameter), "SendBlob");
    return SendToReadyPeers(MakeBlob(size), "blob " + std::to_string(size) + " B (byte i = (i*7+3) mod 256)");
}

int Server::ListPeers() {
    const auto ids = PeerIds();
    Log("peers", std::to_string(ids.size()) + " BleLink peer(s), " + std::to_string(ConnectedCount()) +
        " connected central(s)");
    for (const auto& id : ids) {
        BleLinkPeerInfoC i{};
        i.size = sizeof(i);
        if (BleLink_GetPeerInfo(link_, id.c_str(), &i) != 0) continue;
        const std::string mtu = i.osLimit ? std::to_string(i.osLimit + 3) : std::string("not reported");
        Log("peers", id + ": " + (i.ready ? "READY" : (i.subscribed ? "handshaking (no HELLO yet)" : "not subscribed")) +
            ", packet size " + std::to_string(i.txPacketSize) + " B, MTU " + mtu +
            ", in flight " + std::to_string(i.inFlight) + ", queued " + std::to_string(i.queued) +
            ", receiving " + std::to_string(i.receiving) +
            (i.ready ? ", peer maxPacket " + std::to_string(i.peerMaxPacket) + " maxMessage " +
                std::to_string(i.peerMaxMessage) + " window " + std::to_string(i.peerWindow) +
                " flags " + std::to_string(i.peerFlags) + ", T " + Seconds(i.timeoutMs) : std::string()));
    }
    return static_cast<int>(ids.size());
}

int Server::PumpLink() {
    if (!link_) return 0;
    int handled = 0;
    Bytes buf;
    for (;;) {
        const int size = BleLink_GetEventPayloadSize(link_);
        if (size < 0) break;
        buf.resize(static_cast<size_t>(size));
        BleLinkEventC ev{};
        ev.size = sizeof(ev);
        if (BleLink_PollEvent(link_, &ev, buf.empty() ? nullptr : buf.data(), size) != 1) break;
        ++handled;
        const std::string dev(ev.deviceId, ev.deviceIdLength);
        const uint64_t now = NowSteadyMs();
        switch (ev.type) {
            case BLELINK_EVENT_PEER_READY: {
                const auto& i = ev.info;
                Log("link", "peer " + dev + " READY: packet size " + std::to_string(i.txPacketSize) + " B (MTU " +
                    (i.osLimit ? std::to_string(i.osLimit + 3) : std::string("not reported")) + "), peer maxPacket " +
                    std::to_string(i.peerMaxPacket) + ", maxMessage " + std::to_string(i.peerMaxMessage) +
                    ", window " + std::to_string(i.peerWindow) + ", flags " + std::to_string(i.peerFlags) +
                    ", T " + Seconds(i.timeoutMs));
                break;
            }
            case BLELINK_EVENT_PEER_GONE:
                Log("link", "peer " + dev + " gone (" + ReasonName(ev.reason) + ")");
                for (auto it = incoming_.begin(); it != incoming_.end();)
                    it = it->first.first == dev ? incoming_.erase(it) : std::next(it);
                break;
            case BLELINK_EVENT_MESSAGE_RECEIVED: {
                const size_t n = buf.size();
                const auto key = std::make_pair(dev, ev.wireMsgId);
                std::string rate = "n/a (< 64 KiB: no PROGRESS events to time it)";
                auto it = incoming_.find(key);
                if (it != incoming_.end()) {
                    rate = Rate(n - it->second.bytes, now - it->second.ms) + " (timed from the first 64 KiB mark)";
                    incoming_.erase(it);
                }
                char crc[16];
                std::snprintf(crc, sizeof(crc), "%08X", Crc32(buf.data(), n));
                Log("recv", "message from " + dev + " (wire id " + std::to_string(ev.wireMsgId) + "): " +
                    std::to_string(n) + " B, CRC ok (crc32=" + crc + "), throughput " + rate +
                    ", text=\"" + ToUtf8Text(buf.data(), std::min<size_t>(n, 64)) + "\"" + (n > 64 ? " ..." : ""));
                if (options_.echoMessages) SendTo(dev, buf, true);
                break;
            }
            case BLELINK_EVENT_MESSAGE_SENT: {
                auto it = outgoing_.find(ev.messageId);
                if (it == outgoing_.end()) {
                    Log("send", "message " + std::to_string(ev.messageId) + " to " + dev + " delivered (ACK ok)");
                    break;
                }
                const uint64_t ms = now - it->second.startMs;
                Log(it->second.echo ? "echo" : "send", "message " + std::to_string(ev.messageId) + " to " + dev +
                    " delivered (ACK ok): " + std::to_string(it->second.size) + " B in " + Seconds(ms) + ", " +
                    Rate(it->second.size, ms));
                outgoing_.erase(it);
                break;
            }
            case BLELINK_EVENT_MESSAGE_FAILED: {
                auto it = outgoing_.find(ev.messageId);
                const std::string size = it != outgoing_.end() ? std::to_string(it->second.size) + " B" : "?";
                Log("error", "message " + std::to_string(ev.messageId) + " to " + dev + " (" + size + ") failed: " +
                    ReasonName(ev.reason) + (ev.ackStatus ? ", ACK status " + std::to_string(ev.ackStatus) : ""));
                if (it != outgoing_.end()) outgoing_.erase(it);
                break;
            }
            case BLELINK_EVENT_PROGRESS: {
                // Progress comes at packet boundaries about every 64 KiB; log when a kProgressLogStep
                // boundary was crossed since the last event of the same kind (or at the end).
                uint64_t dummy = 0;
                uint64_t* last = &dummy;
                if (ev.progressKind == BLELINK_PROGRESS_RECEIVING) {
                    const auto key = std::make_pair(dev, ev.wireMsgId);
                    auto it = incoming_.find(key);
                    if (it == incoming_.end()) it = incoming_.emplace(key, IncomingStart{now, ev.bytes, 0}).first;
                    last = &it->second.last;
                } else {
                    auto it = outgoing_.find(ev.messageId);
                    if (it != outgoing_.end())
                        last = ev.progressKind == BLELINK_PROGRESS_SENDING ? &it->second.lastSending : &it->second.lastPeerReceived;
                }
                const uint64_t step = TestProfile::kProgressLogStep;
                const bool logIt = options_.verbose || ev.bytes == ev.total || ev.bytes / step != *last / step;
                *last = ev.bytes;
                if (!logIt) break;
                const char* kind = ev.progressKind == BLELINK_PROGRESS_SENDING ? "sending"
                    : ev.progressKind == BLELINK_PROGRESS_PEER_RECEIVED ? "peer received" : "receiving";
                const std::string which = ev.progressKind == BLELINK_PROGRESS_RECEIVING
                    ? "incoming wire id " + std::to_string(ev.wireMsgId) + " from " + dev
                    : "message " + std::to_string(ev.messageId) + " to " + dev;
                Log("progress", which + ": " + kind + " " + std::to_string(ev.bytes) + "/" + std::to_string(ev.total) + " B");
                break;
            }
            default:
                LogVerbose("link", "event type " + std::to_string(ev.type) + " from " + dev);
                break;
        }
    }
    return handled;
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
    Log("write", "write on " + std::string(CharName(chr)) + " from " + device + ": " + std::to_string(data.size()) +
        " B hex=" + ToHex(data.data(), data.size()) + " text=\"" + ToUtf8Text(data.data(), data.size()) + "\"");
    if (chr == TestProfile::kEchoUuid) {
        if (data.empty()) { Log("echo", "empty write, nothing to echo"); return; }
        if (Notify(TestProfile::kEchoUuid, data, "echo") == 0) {
            Log("notify", "echo -> notify back " + std::to_string(data.size()) + " B");
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
    // Backends also report state changes and diagnostics through this callback with code Success:
    // those are information, not errors.
    if (error == 0) self->Log("backend", message ? message : "");
    else self->Log("error", "BleError " + CodeText(error) + ": " + (message ? message : ""));
}

}  // namespace TestServerApp
