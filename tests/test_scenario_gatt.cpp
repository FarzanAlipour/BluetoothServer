// GATT test-profile scenario against the fake backend. Runs the SAME server code as the
// ble_test_server CLI (examples/ble_test_server/TestServerApp.cpp) and plays the central's role
// through the fake's Simulate* hooks:
//   init -> profile -> advertise -> connect -> read -> write -> subscribe/heartbeat -> echo
//   -> chunked large message (both directions) -> disconnect -> reconnect -> stop -> shutdown.
// Real advertising, MTU limits and OS permissions are NOT covered: that is the hardware test (docs/TESTING.md).
#include "IBleServer.h"
#include "FakeBleServer.h"
#include "TestServerApp.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <string>
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
    const Bytes big = TestServerApp::MakeLargePayload();
    CHECK(big.size() == TestProfile::kLargeMessageSize);
    CHECK(big[0] == 'A' && big[26] == 'A' && big[25] == 'Z');
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
    CHECK(fake->GetServices().size() == 1);
    CHECK(log.Has("Advertising as \"BLE-TestServer\""));
    CHECK(log.Has("Profile registered"));
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

    // ---- large message out: chunks reassemble to the payload at the "central"; ACKs written back
    CHECK(app.SendLargeMessage() == 0);
    auto chunks = NotifiesOn(fake, TestProfile::kLargeUuid);
    CHECK(chunks.size() == 5);  // 1000 B = 4 x 243 + 28
    void* central = BleChunker_Create();
    std::vector<Bytes> acks;
    for (auto& c : chunks) {
        CHECK(c.size() <= BleMessageChunker::CHUNK_SIZE);
        CHECK(!c.empty() && c[0] == 0x01);
        uint8_t ack[64]; int ackLen = sizeof(ack);
        CHECK(BleChunker_ProcessIncoming(central, c.data(), static_cast<int>(c.size()), ack, &ackLen) >= 0);
        acks.emplace_back(ack, ack + ackLen);
    }
    int size = BleChunker_GetNextMessageSize(central);
    Bytes assembled(static_cast<size_t>(size > 0 ? size : 0));
    int len = size;
    CHECK(size == static_cast<int>(TestProfile::kLargeMessageSize));
    CHECK(BleChunker_ReadNextMessage(central, assembled.data(), &len));
    CHECK(assembled == TestServerApp::MakeLargePayload());
    for (auto& a : acks) CHECK(fake->SimulateWrite(kSvc, TestProfile::kLargeUuid, kDev, a));
    CHECK(log.Has("5/5 chunks acknowledged"));
    CHECK(app.RetransmitTick() == 0);  // everything acknowledged: nothing to resend
    CHECK(!log.Has("retransmitted"));

    // ---- large message in: the central sends a chunked message, the server reassembles it
    Bytes inbound(600);
    for (size_t i = 0; i < inbound.size(); ++i) inbound[i] = static_cast<uint8_t>('a' + i % 26);
    auto inChunks = BleMessageChunker::CreateChunks(inbound);
    CHECK(inChunks.size() == 3);
    for (auto& c : inChunks) CHECK(fake->SimulateWrite(kSvc, TestProfile::kLargeUuid, kDev, c));
    CHECK(log.Has("received chunk 3/3"));
    CHECK(log.Has("reassembled: 600 B"));
    CHECK(fake->SimulateWrite(kSvc, TestProfile::kLargeUuid, kDev, Bytes{9, 9}));  // garbage is rejected, not fatal
    CHECK(log.Has("not a valid chunk"));
    BleChunker_Destroy(central);

    // ---- errors surface with the BleError code
    fake->SimulateError(BleError::AdvertisingFailed, "radio busy");
    CHECK(log.Has("[error] BleError AdvertisingFailed (8): radio busy"));

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

}  // namespace

int main() {
    TestHelpers();
    TestScenario();
    TestCustomNameAndRestart();
    std::printf("scenario_gatt: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
