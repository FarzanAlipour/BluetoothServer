// GATT test-profile scenario against the fake backend. Runs the SAME server code as the
// ble_test_server CLI (examples/ble_test_server/TestServerApp.cpp) and plays the central's role
// through the fake's Simulate* hooks:
//   init -> profile -> advertise -> connect -> read -> write -> subscribe/heartbeat -> echo
//   -> disconnect -> reconnect -> stop -> shutdown,
// and the BleLink part: m / b / p commands and --echo-messages against a client-role BleLink engine,
// plus the hand-typed nRF Connect hex from docs/TESTING.md byte for byte.
// Real advertising, MTU limits and OS permissions are NOT covered: that is the hardware test (docs/TESTING.md).
#include "IBleServer.h"
#include "BleLinkEngine.h"
#include "FakeBleServer.h"
#include "TestServerApp.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace BleServer;
using TestServerApp::Bytes;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

const std::string kSvc = TestProfile::kServiceUuid;
const std::string kDev = "AA:BB:CC:00:00:01";

struct LogCapture {
    std::mutex m;
    std::vector<std::string> lines;
    void Add(const std::string& l) { std::lock_guard<std::mutex> g(m); lines.push_back(l); }
    bool Has(const std::string& needle) {
        std::lock_guard<std::mutex> g(m);
        return std::any_of(lines.begin(), lines.end(),
                           [&](const std::string& l) { return l.find(needle) != std::string::npos; });
    }
    size_t Count(const std::string& needle) {
        std::lock_guard<std::mutex> g(m);
        return static_cast<size_t>(std::count_if(lines.begin(), lines.end(),
            [&](const std::string& l) { return l.find(needle) != std::string::npos; }));
    }
};

Bytes B(const std::string& s) { return Bytes(s.begin(), s.end()); }
std::string S(const Bytes& b) { return std::string(b.begin(), b.end()); }

// Notifications on one characteristic, in order.
std::vector<Bytes> NotifiesOn(FakeBleServer* f, const char* chr) {
    std::vector<Bytes> out;
    for (auto& e : f->NotifyLog()) {
        std::string c = e.characteristic;
        std::string want = chr;
        std::transform(c.begin(), c.end(), c.begin(), ::tolower);
        std::transform(want.begin(), want.end(), want.begin(), ::tolower);
        if (c == want) out.push_back(e.value);
    }
    return out;
}

void TestHelpers() {
    const uint8_t raw[] = {0x48, 0x69, 0xFF, 0xFE};
    CHECK(TestServerApp::ToHex(raw, 4) == "48 69 FF FE");
    CHECK(TestServerApp::ToHex(raw, 0).empty());
    CHECK(TestServerApp::ToUtf8Text(raw, 2) == "Hi");
    CHECK(TestServerApp::ToUtf8Text(raw + 2, 2) == "\xEF\xBF\xBD\xEF\xBF\xBD");  // invalid bytes -> U+FFFD
    const uint8_t utf8[] = {0xC3, 0xA9};                                           // "é"
    CHECK(TestServerApp::ToUtf8Text(utf8, 2) == "\xC3\xA9");
    // Blob = payloadRule of docs/protocol-test-vectors.json: byte i = (i * 7 + 3) mod 256.
    const Bytes blob = TestServerApp::MakeBlob(300);
    CHECK(blob.size() == 300);
    CHECK(blob[0] == 3 && blob[1] == 10 && blob[36] == static_cast<uint8_t>(36 * 7 + 3) && blob[256] == 3);
    CHECK(TestServerApp::MakeBlob(0).empty());
    // CRC-32/IEEE check value (PROTOCOL.md 2.4) and the "hello" CRC used in docs/TESTING.md.
    const std::string check = "123456789";
    CHECK(TestServerApp::Crc32(reinterpret_cast<const uint8_t*>(check.data()), check.size()) == 0xCBF43926u);
    CHECK(TestServerApp::Crc32(reinterpret_cast<const uint8_t*>("hello"), 5) == 0x3610A686u);
    CHECK(TestServerApp::Crc32(nullptr, 0) == 0);
    size_t n = 0;
    CHECK(TestServerApp::ParseSize("1000", &n) && n == 1000);
    CHECK(TestServerApp::ParseSize("64k", &n) && n == 65536);
    CHECK(TestServerApp::ParseSize("64K", &n) && n == 65536);
    CHECK(TestServerApp::ParseSize(" 1M ", &n) && n == 1048576);
    CHECK(TestServerApp::ParseSize("1.5m", &n) && n == 1572864);
    CHECK(TestServerApp::ParseSize("0", &n) && n == 0);
    CHECK(!TestServerApp::ParseSize("", &n));
    CHECK(!TestServerApp::ParseSize("k", &n));
    CHECK(!TestServerApp::ParseSize("-1", &n));
    CHECK(!TestServerApp::ParseSize("12x", &n));
    CHECK(!TestServerApp::ParseSize("1e3", &n));
    CHECK(!TestServerApp::ParseSize("4096M", &n));  // above kMaxBlobSize
    CHECK(!TestServerApp::ParseSize("10", nullptr));
}

// Calls app.PumpLink() (what main.cpp does every 10 ms) and `step` until `done` or timeout.
bool PumpUntil(TestServerApp::Server& app, const std::function<bool()>& done,
               const std::function<void()>& step = nullptr, int timeoutMs = 20000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        if (step) step();
        app.PumpLink();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void TestScenario() {
    LogCapture log;
    TestServerApp::Options opt;  // default name BLE-TestServer
    TestServerApp::Server app(opt, [&](const std::string& l) { log.Add(l); });

    // ---- init -> add profile -> advertise
    CHECK(app.Start());
    FakeBleServer* fake = FakeBleServer::Last();
    CHECK(fake != nullptr);
    if (!fake) return;
    CHECK(fake->IsInitialized());
    CHECK(fake->IsAdvertising());
    CHECK(app.IsAdvertising());
    CHECK(fake->GetServices().size() == 2);  // the test profile and BleLink
    CHECK(log.Has("Advertising as \"BLE-TestServer\""));
    CHECK(log.Has("Profile registered: service " + kSvc + ", 4 characteristics"));
    CHECK(log.Has(std::string("BleLink service ") + BLELINK_SERVICE_UUID + " registered"));
    CHECK(app.Link() != nullptr);
    CHECK(app.Name() == "BLE-TestServer");

    // Heartbeat with nobody connected is a no-op.
    CHECK(app.SendHeartbeat() == 0);
    CHECK(NotifiesOn(fake, TestProfile::kNotifyUuid).empty());

    // ---- simulated connect
    fake->SimulateConnection(kDev, true);
    CHECK(app.ConnectedCount() == 1);
    CHECK(log.Has("[connect]") && log.Has(kDev));

    // ---- read: "uptime=<s>s read#<n>", the counter counts central reads (one read = one count)
    Bytes v;
    CHECK(fake->SimulateRead(kSvc, TestProfile::kReadUuid, kDev, v));
    CHECK(S(v).rfind("uptime=", 0) == 0);
    CHECK(S(v).find("s read#1") != std::string::npos);
    CHECK(fake->SimulateRead(kSvc, TestProfile::kReadUuid, kDev, v));
    CHECK(S(v).find("s read#2") != std::string::npos);
    CHECK(log.Count("[read]") == 2);

    // ---- write: logged as hex + UTF-8
    CHECK(fake->SimulateWrite(kSvc, TestProfile::kWriteUuid, kDev, B("Hello")));
    CHECK(log.Has("hex=48 65 6C 6C 6F") && log.Has("text=\"Hello\""));
    CHECK(fake->SimulateWrite(kSvc, TestProfile::kWriteUuid, kDev, Bytes{0xDE, 0xAD, 0xBE, 0xEF}));
    CHECK(log.Has("hex=DE AD BE EF"));
    CHECK(log.Has("\xEF\xBF\xBD"));  // the invalid UTF-8 bytes (BE, EF) show as replacement characters
    CHECK(NotifiesOn(fake, TestProfile::kWriteUuid).empty());  // plain write does not echo

    // ---- subscribe: heartbeat notifies (numbered), then a manual one
    for (int i = 0; i < 3; ++i) CHECK(app.SendHeartbeat() == 0);
    auto hb = NotifiesOn(fake, TestProfile::kNotifyUuid);
    CHECK(hb.size() == 3);
    if (hb.size() == 3) {
        CHECK(S(hb[0]) == "heartbeat #1");
        CHECK(S(hb[2]) == "heartbeat #3");
    }
    CHECK(app.SendManualNotify() == 0);
    hb = NotifiesOn(fake, TestProfile::kNotifyUuid);
    CHECK(hb.size() == 4 && S(hb.back()) == "manual #1");

    // ---- echo: whatever is written comes back as a notification
    CHECK(fake->SimulateWrite(kSvc, TestProfile::kEchoUuid, kDev, B("ping")));
    CHECK(fake->SimulateWrite(kSvc, TestProfile::kEchoUuid, kDev, Bytes{1, 2, 3}));
    auto echo = NotifiesOn(fake, TestProfile::kEchoUuid);
    CHECK(echo.size() == 2);
    if (echo.size() == 2) {
        CHECK(S(echo[0]) == "ping");
        CHECK(echo[1] == (Bytes{1, 2, 3}));
    }
    CHECK(fake->SimulateWrite(kSvc, TestProfile::kEchoUuid, kDev, Bytes{}));  // empty: nothing to echo
    CHECK(NotifiesOn(fake, TestProfile::kEchoUuid).size() == 2);

    // ---- the legacy chunker characteristic is gone; BleLink commands without a BleLink peer
    CHECK(!fake->SimulateWrite(kSvc, "7b1e0005-5a6c-4f0e-9d3b-2c1a8e4f6b01", kDev, Bytes{1}));
    CHECK(app.SendText("nobody listens") == static_cast<int>(BleError::PeerNotReady));
    CHECK(app.SendBlob(1000) == static_cast<int>(BleError::PeerNotReady));
    CHECK(log.Has("no READY BleLink peer"));
    // Connected, not subscribed to TX: a BleLink session without HELLO. The engine applies the
    // connect on its worker thread, so wait for it instead of reading the peer list right away.
    CHECK(PumpUntil(app, [&] { return app.ListPeers() == 1; }));
    CHECK(log.Has("[peers] 1 BleLink peer(s), 1 connected central(s)"));
    CHECK(log.Has("[peers] " + kDev + ": not subscribed, packet size 20 B, MTU not reported, in flight 0"));
    CHECK(app.ReadyPeerCount() == 0);
    CHECK(app.PumpLink() == 0);

    // ---- errors surface with the BleError code; backend information (code Success) is not an error
    fake->SimulateError(BleError::AdvertisingFailed, "radio busy");
    CHECK(log.Has("[error] BleError AdvertisingFailed (8): radio busy"));
    fake->SimulateError(BleError::Success, "Bluetooth state: PoweredOn");
    CHECK(log.Has("[backend] Bluetooth state: PoweredOn"));
    CHECK(!log.Has("BleError Success"));

    // ---- disconnect -> heartbeat stops -> reconnect -> everything works again
    fake->SimulateConnection(kDev, false);
    CHECK(app.ConnectedCount() == 0);
    CHECK(log.Has("[disconnect]"));
    const size_t notifiesBefore = fake->NotifyLog().size();
    CHECK(app.SendHeartbeat() == 0);
    CHECK(fake->NotifyLog().size() == notifiesBefore);

    fake->SimulateConnection(kDev, true);
    CHECK(app.ConnectedCount() == 1);
    CHECK(app.SendHeartbeat() == 0);
    CHECK(fake->NotifyLog().size() == notifiesBefore + 1);
    CHECK(fake->SimulateRead(kSvc, TestProfile::kReadUuid, kDev, v));
    CHECK(S(v).find("s read#3") != std::string::npos);
    CHECK(fake->SimulateWrite(kSvc, TestProfile::kEchoUuid, kDev, B("again")));
    echo = NotifiesOn(fake, TestProfile::kEchoUuid);
    CHECK(echo.size() == 3 && S(echo.back()) == "again");

    // ---- stop/start advertising (the "s" command), then stop
    CHECK(app.ToggleAdvertising() == 0);
    CHECK(!app.IsAdvertising());
    CHECK(app.ToggleAdvertising() == 0);
    CHECK(app.IsAdvertising());
    fake->SimulateConnection(kDev, false);

    // ---- shutdown: backend gone, nothing runs afterwards
    app.Stop();
    CHECK(FakeBleServer::Last() == nullptr);
    CHECK(log.Has("Server shut down"));
    CHECK(app.SendManualNotify() != 0);  // NotInitialized after Stop, no crash
    app.Stop();                          // idempotent
}

void TestCustomNameAndRestart() {
    LogCapture log;
    TestServerApp::Options opt;
    opt.name = "Custom-Name";
    opt.verbose = true;
    {
        TestServerApp::Server app(opt, [&](const std::string& l) { log.Add(l); });
        CHECK(app.Start());
        CHECK(log.Has("Advertising as \"Custom-Name\""));
        CHECK(log.Has("[verbose]"));
        // destroyed while running: the destructor must shut everything down
    }
    CHECK(FakeBleServer::Last() == nullptr);
    CHECK(log.Has("Server shut down"));
    // A second server instance in the same process starts cleanly.
    TestServerApp::Server again(opt, [&](const std::string& l) { log.Add(l); });
    CHECK(again.Start());
    again.Stop();
}

// ---- BleLink -----------------------------------------------------------------------------------

Bytes Hex(const std::string& hex) {
    Bytes out;
    std::string digits;
    for (char c : hex) if (std::isxdigit(static_cast<unsigned char>(c))) digits += c;
    for (size_t i = 0; i + 1 < digits.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoul(digits.substr(i, 2), nullptr, 16)));
    return out;
}

// TX notifications the server sent to one central.
std::vector<Bytes> TxTo(FakeBleServer* f, const std::string& dev) {
    std::vector<Bytes> out;
    for (auto& e : f->PeerNotifyLog())
        if (e.device == dev && e.characteristic == BLELINK_TX_UUID) out.push_back(e.value);
    return out;
}

// A real BleLink central (the client-role engine, as in tests/test_blelink_abi.cpp) on the fake.
class EngineCentral : public BleLink::ITransport {
public:
    EngineCentral(FakeBleServer* fake, std::string dev) : fake_(fake), dev_(std::move(dev)), engine_(*this, Config()) {
        engine_.SetPeerOsLimit(kServer, 244);
    }
    static BleLink::Config Config() {
        BleLink::Config c;
        c.role = BleLink::Role::Client;
        return c;
    }
    BleLink::TransportResult SendPacket(const BleLink::PeerId&, const uint8_t* data, size_t len) override {
        fake_->SimulateWriteEx(BLELINK_SERVICE_UUID, BLELINK_RX_UUID, dev_, Bytes(data, data + len), false);
        return BleLink::TransportResult::Sent;
    }
    void Connect(uint16_t mtu) {
        fake_->SimulateConnection(dev_, true);
        fake_->SimulateMtu(dev_, mtu);
        fake_->SimulateSubscription(BLELINK_SERVICE_UUID, BLELINK_TX_UUID, dev_, true);
        engine_.OnPeerConnected(kServer);
        engine_.OnPeerSubscribed(kServer);  // sends HELLO
    }
    void Step() {
        auto log = fake_->PeerNotifyLog();
        for (; seen_ < log.size(); ++seen_)
            if (log[seen_].device == dev_) engine_.OnPacket(kServer, log[seen_].value.data(), log[seen_].value.size());
        engine_.Pump();
        BleLink::Event ev;
        while (engine_.PollEvent(ev)) {
            if (ev.type == BleLink::EventType::PeerReady) ready = true;
            if (ev.type == BleLink::EventType::MessageReceived) received.push_back(ev.data);
            if (ev.type == BleLink::EventType::MessageSent) ++sent;
        }
    }
    BleLink::Engine& Engine() { return engine_; }
    bool ready = false;
    int sent = 0;
    std::vector<Bytes> received;
    static constexpr const char* kServer = "server";

private:
    FakeBleServer* fake_;
    std::string dev_;
    size_t seen_ = 0;
    BleLink::Engine engine_;
};

// m / b / p commands, received-message logging and --echo-messages with a protocol-correct central.
void TestBleLinkCommandsAndEcho() {
    LogCapture log;
    TestServerApp::Options opt;
    opt.echoMessages = true;
    TestServerApp::Server app(opt, [&](const std::string& l) { log.Add(l); });
    CHECK(app.Start());
    FakeBleServer* fake = FakeBleServer::Last();
    CHECK(fake != nullptr);
    if (!fake) return;
    CHECK(log.Has("echo mode ON"));

    const std::string dev = "CENTRAL-1";
    EngineCentral c(fake, dev);
    c.Connect(247);
    auto step = [&] { c.Step(); };
    CHECK(PumpUntil(app, [&] { return c.ready && log.Has("[link] peer " + dev + " READY"); }, step));
    CHECK(log.Has("READY: packet size 244 B (MTU 247), peer maxPacket 512, maxMessage 16777216, window 4"));
    CHECK(app.ReadyPeerCount() == 1);
    CHECK(app.ListPeers() == 1);
    CHECK(log.Has("[peers] " + dev + ": READY, packet size 244 B, MTU 247, in flight 0, queued 0"));

    // m <text> -> the central receives exactly that message; the ACK is logged with a throughput.
    CHECK(app.SendText("hi there") == 0);
    CHECK(log.Has("text \"hi there\" (8 B): queued for 1/1 ready peer(s)"));
    CHECK(PumpUntil(app, [&] { return log.Has("delivered (ACK ok): 8 B"); }, step));
    CHECK(c.received.size() == 1 && c.received[0] == B("hi there"));

    // b 1M -> 1 MiB blob with the payloadRule, progress logged at 256 KiB steps.
    CHECK(app.SendBlob(1024 * 1024) == 0);
    CHECK(PumpUntil(app, [&] { return log.Has("delivered (ACK ok): 1048576 B"); }, step, 60000));
    CHECK(c.received.size() == 2 && c.received.back() == TestServerApp::MakeBlob(1024 * 1024));
    // 16 PROGRESS events per kind; only the 256 KiB crossings and the end are logged (no --verbose).
    CHECK(log.Count("/1048576 B") > 0);
    CHECK(log.Count(": sending ") == 4);
    CHECK(log.Has(": sending 1048576/1048576 B"));
    CHECK(log.Count(": peer received ") <= 4);
    CHECK(log.Has("KB/s"));

    // m alone = an empty message.
    CHECK(app.SendText("") == 0);
    CHECK(PumpUntil(app, [&] { return c.received.size() == 3; }, step));
    CHECK(c.received.size() == 3 && c.received.back().empty());

    // Central -> server: small text, then 200 KB; both logged with CRC and echoed back unchanged.
    CHECK(c.Engine().Send(EngineCentral::kServer, B("ping from central")).code == BleLink::SendOutcome::Code::Ok);
    CHECK(PumpUntil(app, [&] { return c.received.size() == 4 && c.sent == 1; }, step));
    CHECK(c.received.size() == 4 && c.received.back() == B("ping from central"));
    CHECK(log.Has("[recv] message from " + dev + " (wire id 0): 17 B, CRC ok (crc32="));
    CHECK(log.Has("text=\"ping from central\""));
    CHECK(log.Has("throughput n/a (< 64 KiB"));
    CHECK(log.Has("[echo] message"));

    Bytes up(200 * 1024);
    for (size_t i = 0; i < up.size(); ++i) up[i] = static_cast<uint8_t>('a' + i % 26);
    CHECK(c.Engine().Send(EngineCentral::kServer, up).code == BleLink::SendOutcome::Code::Ok);
    CHECK(PumpUntil(app, [&] { return c.received.size() == 5; }, step, 60000));
    CHECK(c.received.size() == 5 && c.received.back() == up);
    CHECK(log.Has("204800 B, CRC ok"));
    CHECK(log.Has("(timed from the first 64 KiB mark)"));
    CHECK(log.Has("text=\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijkl\" ..."));  // first 64 bytes
    CHECK(PumpUntil(app, [&] { return log.Count("[echo] message") >= 4; }, step));  // 2 queued + 2 delivered lines
    CHECK(!log.Has("[error]"));

    // Central leaves: PEER_GONE logged, commands report no peer again.
    fake->SimulateSubscription(BLELINK_SERVICE_UUID, BLELINK_TX_UUID, dev, false);
    fake->SimulateConnection(dev, false);
    CHECK(PumpUntil(app, [&] { return log.Has("[link] peer " + dev + " gone"); }));
    CHECK(app.SendText("late") == static_cast<int>(BleError::PeerNotReady));
    app.Stop();
}

// The exact hex documented in docs/TESTING.md for testing BleLink by hand in nRF Connect (no MTU
// exchange: packet size 20, HELLO + single-packet messages + hand-written ACK).
void TestBleLinkByHandHex() {
    LogCapture log;
    TestServerApp::Options opt;
    opt.echoMessages = true;
    TestServerApp::Server app(opt, [&](const std::string& l) { log.Add(l); });
    CHECK(app.Start());
    FakeBleServer* fake = FakeBleServer::Last();
    CHECK(fake != nullptr);
    if (!fake) return;
    const std::string dev = "NRF-PHONE";
    Bytes info;
    CHECK(fake->SimulateRead(BLELINK_SERVICE_UUID, BLELINK_INFO_UUID, dev, info));
    CHECK(TestServerApp::ToHex(info.data(), info.size()) == "01 00 02 00 00 00 01 04 01 00");  // GATT_TEST_PROFILE.md
    fake->SimulateConnection(dev, true);
    fake->SimulateSubscription(BLELINK_SERVICE_UUID, BLELINK_TX_UUID, dev, true);  // enable TX notifications
    CHECK(PumpUntil(app, [&] { app.ListPeers(); return log.Has("[peers] " + dev + ": handshaking (no HELLO yet)"); }));

    // 1. HELLO: version 1, maxPacket 244, maxMessage 1024, window 1, no PROGRESS, timeout 240 x 0.25 s = 60 s.
    CHECK(fake->SimulateWriteEx(BLELINK_SERVICE_UUID, BLELINK_RX_UUID, dev, Hex("01 00 01 F4 00 00 04 00 00 01 00 F0"), true));
    CHECK(PumpUntil(app, [&] { return log.Has("[link] peer " + dev + " READY"); }));
    CHECK(log.Has("READY: packet size 20 B (MTU not reported), peer maxPacket 244, maxMessage 1024, window 1, flags 0, T 60.00 s"));
    auto tx = TxTo(fake, dev);
    CHECK(tx.size() == 1);
    if (!tx.empty()) {
        // The server's HELLO as nRF Connect shows it (documented in TESTING.md).
        CHECK(TestServerApp::ToHex(tx[0].data(), tx[0].size()) == "01 00 01 00 02 00 00 00 01 04 01 00");
    }

    // 2. MSG_START seq 1, msgId 0, 5 bytes "hello" (crc32 3610A686) -> ACK seq 1 msgId 0 status 0.
    CHECK(fake->SimulateWriteEx(BLELINK_SERVICE_UUID, BLELINK_RX_UUID, dev,
                                Hex("02 01 00 00 05 00 00 00 86 A6 10 36 68 65 6C 6C 6F"), true));
    CHECK(PumpUntil(app, [&] { return TxTo(fake, dev).size() >= 3; }));
    CHECK(log.Has("[recv] message from " + dev + " (wire id 0): 5 B, CRC ok (crc32=3610A686)"));
    tx = TxTo(fake, dev);
    if (tx.size() >= 3) {
        // The ACK comes first (sent on delivery); the echo is queued only after PumpLink saw the message.
        CHECK(TestServerApp::ToHex(tx[1].data(), tx[1].size()) == "04 01 00 00 00");
        CHECK(TestServerApp::ToHex(tx[2].data(), tx[2].size()) == "02 02 00 00 05 00 00 00 86 A6 10 36 68 65 6C 6C 6F");
    }

    // 3. ACK the echoed message by hand: seq 2, msgId 0, status 0.
    CHECK(fake->SimulateWriteEx(BLELINK_SERVICE_UUID, BLELINK_RX_UUID, dev, Hex("04 02 00 00 00"), true));
    CHECK(PumpUntil(app, [&] { return log.Has("[echo] message 1 to " + dev + " delivered (ACK ok): 5 B"); }));

    // 4. `m hi` from the console: one MSG_START "hi" (14 B), ACKed by hand with seq 3, msgId 1.
    CHECK(app.SendText("hi") == 0);
    CHECK(PumpUntil(app, [&] { return TxTo(fake, dev).size() >= 4; }));
    tx = TxTo(fake, dev);
    if (tx.size() >= 4) {
        CHECK(TestServerApp::ToHex(tx[3].data(), tx[3].size()) == "02 03 01 00 02 00 00 00 AC 2A 93 D8 68 69");
    }
    CHECK(fake->SimulateWriteEx(BLELINK_SERVICE_UUID, BLELINK_RX_UUID, dev, Hex("04 03 01 00 00"), true));
    CHECK(PumpUntil(app, [&] { return log.Has("[send] message 2 to " + dev + " delivered (ACK ok): 2 B"); }));
    CHECK(!log.Has("[error]"));

    // 5. A message larger than the hand-written HELLO's maxMessage is refused by Send (no crash).
    CHECK(app.SendBlob(2000) == static_cast<int>(BleError::MessageTooLarge));
    CHECK(log.Has("failed: MessageTooLarge (11)"));
    app.Stop();
    CHECK(log.Has("Server shut down"));
}

// Shutdown with a message still unacknowledged: the failure is logged, not lost.
void TestBleLinkShutdownWithMessageInFlight() {
    LogCapture log;
    TestServerApp::Options opt;
    TestServerApp::Server app(opt, [&](const std::string& l) { log.Add(l); });
    CHECK(app.Start());
    FakeBleServer* fake = FakeBleServer::Last();
    if (!fake) { CHECK(false); return; }
    const std::string dev = "NRF-2";
    fake->SimulateConnection(dev, true);
    fake->SimulateSubscription(BLELINK_SERVICE_UUID, BLELINK_TX_UUID, dev, true);
    CHECK(fake->SimulateWriteEx(BLELINK_SERVICE_UUID, BLELINK_RX_UUID, dev, Hex("01 00 01 F4 00 00 04 00 00 01 00 F0"), true));
    CHECK(PumpUntil(app, [&] { return app.ReadyPeerCount() == 1; }));
    CHECK(app.SendText("never acked") == 0);
    CHECK(PumpUntil(app, [&] { return TxTo(fake, dev).size() >= 2; }));
    app.Stop();
    CHECK(log.Has("to " + dev + " (11 B) failed: shutdown"));
}

}  // namespace

int main() {
    TestHelpers();
    TestScenario();
    TestCustomNameAndRestart();
    TestBleLinkCommandsAndEcho();
    TestBleLinkByHandHex();
    TestBleLinkShutdownWithMessageInFlight();
    std::printf("scenario_gatt: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
