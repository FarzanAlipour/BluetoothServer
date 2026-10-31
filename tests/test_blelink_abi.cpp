// BleLink C ABI (API v3) tests: BleLink_* in src/common/BleLinkAbi.cpp on the in-memory FakeBleServer.
// The central is a second BleLink engine (Role::Client): its packets are written to RX through
// FakeBleServer::SimulateWriteEx, and the server's TX notifications (FakeBleServer::PeerNotifyLog) are
// fed back into it. No hardware.
#include "IBleServer.h"
#include "BleLinkCodec.h"
#include "BleLinkEngine.h"
#include "BleServerLinkHooks.h"
#include "FakeBleServer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace BleServer;
namespace Link = BleServer::BleLink;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

using Bytes = std::vector<uint8_t>;
static const int kOk = 0;
static const int kInvalid = static_cast<int>(BleError::InvalidParameter);
static const int kNotInit = static_cast<int>(BleError::NotInitialized);
static const int kNotReady = static_cast<int>(BleError::PeerNotReady);
static const int kTooLarge = static_cast<int>(BleError::MessageTooLarge);
static const int kTooSmall = static_cast<int>(BleError::BufferTooSmall);

static Bytes Pattern(size_t n, uint32_t seed) {
    Bytes b(n);
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        b[i] = static_cast<uint8_t>(x);
    }
    return b;
}

// One polled server event (copied out of BleLinkEventC).
struct ServerEvent {
    BleLinkEventC ev{};
    Bytes data;
    std::string device() const { return std::string(ev.deviceId, ev.deviceIdLength); }
};

// A central speaking BleLink through the fake backend.
class Central : public Link::ITransport {
public:
    Central(FakeBleServer* fake, std::string dev, uint16_t writeLimit = 244)
        : fake_(fake), dev_(std::move(dev)), engine_(*this, ClientConfig()) {
        engine_.SetPeerOsLimit(kServer, writeLimit);
    }
    static Link::Config ClientConfig() {
        Link::Config c;
        c.role = Link::Role::Client;
        c.maxMessage = 16u * 1024u * 1024u;
        c.window = 4;
        return c;
    }
    Link::TransportResult SendPacket(const Link::PeerId&, const uint8_t* data, size_t len) override {
        if (blockWrites) return Link::TransportResult::Busy;
        fake_->SimulateWriteEx(BLELINK_SERVICE_UUID, BLELINK_RX_UUID, dev_, Bytes(data, data + len), false);
        return Link::TransportResult::Sent;
    }
    void Connect(uint16_t mtu = 247) {
        fake_->SimulateConnection(dev_, true);
        fake_->SimulateMtu(dev_, mtu);
        fake_->SimulateSubscription(BLELINK_SERVICE_UUID, BLELINK_TX_UUID, dev_, true);
        engine_.OnPeerConnected(kServer);
        engine_.OnPeerSubscribed(kServer);  // client sends HELLO
    }
    // Feeds the server's new notifications to this central. Returns how many.
    size_t Drain() {
        auto log = fake_->PeerNotifyLog();
        size_t n = 0;
        for (; seen_ < log.size(); ++seen_) {
            const auto& e = log[seen_];
            if (e.device != dev_) continue;
            engine_.OnPacket(kServer, e.value.data(), e.value.size());
            ++n;
        }
        return n;
    }
    void Pump() {
        engine_.Pump();
        Link::Event ev;
        while (engine_.PollEvent(ev)) events.push_back(std::move(ev));
    }
    bool Has(Link::EventType t) const {
        for (const auto& e : events) if (e.type == t) return true;
        return false;
    }
    const Link::Event* Find(Link::EventType t) const {
        for (const auto& e : events) if (e.type == t) return &e;
        return nullptr;
    }
    Link::Engine& Engine() { return engine_; }
    const std::string& Device() const { return dev_; }

    std::vector<Link::Event> events;
    bool blockWrites = false;
    static constexpr const char* kServer = "server";

private:
    FakeBleServer* fake_;
    std::string dev_;
    size_t seen_ = 0;
    Link::Engine engine_;
};

static int PollAll(void* link, std::vector<ServerEvent>& out) {
    int n = 0;
    for (;;) {
        const int size = BleLink_GetEventPayloadSize(link);
        if (size < 0) return n;
        ServerEvent se;
        se.ev.size = sizeof(BleLinkEventC);
        se.data.resize(static_cast<size_t>(size));
        const int r = BleLink_PollEvent(link, &se.ev, se.data.empty() ? nullptr : se.data.data(),
            static_cast<int>(se.data.size()));
        if (r != 1) return n;
        out.push_back(std::move(se));
        ++n;
    }
}

static const ServerEvent* FindEv(const std::vector<ServerEvent>& v, int type, const std::string& dev = "") {
    for (const auto& e : v)
        if (e.ev.type == type && (dev.empty() || e.device() == dev)) return &e;
    return nullptr;
}

// Pumps every central and polls the link until `done` or timeout.
static bool RunUntil(void* link, std::vector<Central*> centrals, std::vector<ServerEvent>& events,
    const std::function<bool()>& done, int timeoutMs = 20000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        for (auto* c : centrals) { c->Drain(); c->Pump(); }
        if (link) PollAll(link, events);
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

static void* MakeServer() {
    void* s = BleServer_Create();
    CHECK(s != nullptr);
    CHECK(BleServer_Initialize(s, "BleLinkTest", false) == kOk);
    return s;
}

// ---------------------------------------------------------------------------------------------------
static void TestVersionAndLayout() {
    CHECK(BleServer_GetApiVersion() == 3);
    CHECK(std::strstr(BleServer_GetBuildInfo(), "api=3") != nullptr);
    CHECK(sizeof(BleLinkConfigC) == 24);
    CHECK(sizeof(BleLinkPeerInfoC) == 56);
    CHECK(sizeof(BleLinkEventC) == 368);
}

static void TestInvalidHandles() {
    int dummy = 0;
    (void)dummy;
    uint32_t id = 123;
    BleLinkEventC ev{};
    ev.size = sizeof(ev);
    BleLinkPeerInfoC info{};
    info.size = sizeof(info);
    uint8_t buf[4];
    int len = 4;
    char text[8];
    void* bogus = &dummy;
    for (void* h : {static_cast<void*>(nullptr), bogus}) {
        CHECK(BleLink_Create(h, nullptr) == nullptr);
        BleLink_Destroy(h);
        CHECK(BleLink_Send(h, "dev", buf, 1, &id) == kInvalid);
        CHECK(id == 0);
        CHECK(BleLink_Cancel(h, 1) == kInvalid);
        CHECK(BleLink_GetEventPayloadSize(h) == -kInvalid);
        CHECK(BleLink_PollEvent(h, &ev, buf, 4) == -kInvalid);
        CHECK(BleLink_GetPeerInfo(h, "dev", &info) == kInvalid);
        CHECK(BleLink_GetPeerCount(h) == -1);
        CHECK(BleLink_GetPeerId(h, 0, text, &len) == kInvalid);
    }
    // A server handle is not a link handle and vice versa.
    void* s = MakeServer();
    CHECK(BleLink_Send(s, "dev", buf, 1, nullptr) == kInvalid);
    void* l = BleLink_Create(s, nullptr);
    CHECK(l != nullptr);
    CHECK(BleServer_StartAdvertising(l) == kInvalid);
    BleLink_Destroy(l);
    BleServer_Destroy(s);
}

static void TestCreateValidation() {
    void* s = BleServer_Create();
    CHECK(BleLink_Create(s, nullptr) == nullptr);  // not initialized
    CHECK(std::strstr(BleServer_GetLastError(s), "not initialized") != nullptr);
    CHECK(BleServer_Initialize(s, "T", false) == kOk);

    BleLinkConfigC c{};
    CHECK(BleLink_Create(s, &c) == nullptr);  // size 0
    c.size = sizeof(c);
    c.window = 256;
    CHECK(BleLink_Create(s, &c) == nullptr);
    c.window = 0;
    c.maxPacket = 19;
    CHECK(BleLink_Create(s, &c) == nullptr);
    c.maxPacket = 513;
    CHECK(BleLink_Create(s, &c) == nullptr);
    c.maxPacket = 0;
    c.maxMessage = 0x80000000u;
    CHECK(BleLink_Create(s, &c) == nullptr);
    c.maxMessage = 0;
    c.timeoutMs = 63751;
    CHECK(BleLink_Create(s, &c) == nullptr);
    c.timeoutMs = 0;
    c.flags = 0x80;
    CHECK(BleLink_Create(s, &c) == nullptr);
    CHECK(std::strstr(BleServer_GetLastError(s), "flags") != nullptr);
    CHECK(BleServer::Internal::LiveLinkHandles() == 0);

    // Older (smaller) struct: trailing fields take defaults, flags 0x80 beyond `size` is ignored.
    c.size = offsetof(BleLinkConfigC, maxPacket);
    c.maxMessage = 1000;
    c.window = 2;
    void* l = BleLink_Create(s, &c);
    CHECK(l != nullptr);
    CHECK(BleLink_Create(s, nullptr) == nullptr);  // one link per server
    CHECK(std::strstr(BleServer_GetLastError(s), "already") != nullptr);

    // INFO value carries our limits (version, maxPacket 512, maxMessage 1000, window 2).
    FakeBleServer* f = FakeBleServer::Last();
    Bytes infoValue;
    CHECK(f->SimulateRead(BLELINK_SERVICE_UUID, BLELINK_INFO_UUID, "dev", infoValue));
    CHECK(infoValue.size() == 10);
    if (infoValue.size() == 10) {
        CHECK(infoValue[0] == 1);
        CHECK((infoValue[1] | (infoValue[2] << 8)) == 512);
        CHECK((infoValue[3] | (infoValue[4] << 8) | (infoValue[5] << 16) | (uint32_t(infoValue[6]) << 24)) == 1000u);
        CHECK(infoValue[7] == 2);
    }
    auto services = f->GetServices();
    bool found = false;
    for (auto& sv : services) found = found || sv == BLELINK_SERVICE_UUID;
    CHECK(found);

    BleLink_Destroy(l);
    BleLink_Destroy(l);  // second destroy: no-op
    services = f->GetServices();
    found = false;
    for (auto& sv : services) found = found || sv == BLELINK_SERVICE_UUID;
    CHECK(!found);  // service removed
    void* l2 = BleLink_Create(s, nullptr);  // slot free again
    CHECK(l2 != nullptr);
    BleLink_Destroy(l2);
    BleServer_Destroy(s);
    CHECK(BleServer::Internal::LiveLinkHandles() == 0);
}

static void TestRoundTrip() {
    void* s = MakeServer();
    void* link = BleLink_Create(s, nullptr);
    CHECK(link != nullptr);
    FakeBleServer* f = FakeBleServer::Last();
    Central c(f, "central-A");
    std::vector<ServerEvent> ev;

    uint8_t one = 1;
    CHECK(BleLink_Send(link, "central-A", &one, 1, nullptr) == kNotReady);  // no session yet

    c.Connect(247);
    CHECK(RunUntil(link, {&c}, ev, [&] {
        return c.Has(Link::EventType::PeerReady) && FindEv(ev, BLELINK_EVENT_PEER_READY, "central-A");
    }));
    const ServerEvent* ready = FindEv(ev, BLELINK_EVENT_PEER_READY, "central-A");
    if (ready) {
        CHECK(ready->ev.info.ready == 1);
        CHECK(ready->ev.info.txPacketSize == 244);  // min(client maxPacket 512, MTU 247 - 3)
        CHECK(ready->ev.info.peerWindow == 4);
        CHECK(ready->ev.info.peerMaxMessage == 16u * 1024u * 1024u);
        CHECK(ready->ev.dataLength == 0);
    }
    BleLinkPeerInfoC info{};
    info.size = sizeof(info);
    CHECK(BleLink_GetPeerInfo(link, "central-A", &info) == kOk);
    CHECK(info.ready == 1 && info.subscribed == 1 && info.osLimit == 244);
    CHECK(BleLink_GetPeerInfo(link, "nobody", &info) == kNotReady);
    info.size = 8;
    CHECK(BleLink_GetPeerInfo(link, "central-A", &info) == kInvalid);
    CHECK(BleLink_GetPeerCount(link) == 1);
    int len = 0;
    CHECK(BleLink_GetPeerId(link, 0, nullptr, &len) == kInvalid && len == 10);
    char id[16];
    len = sizeof(id);
    CHECK(BleLink_GetPeerId(link, 0, id, &len) == kOk && std::string(id) == "central-A");
    CHECK(BleLink_GetPeerId(link, 1, id, &len) == kInvalid);

    // Server -> central: empty, small and 300 KB, with progress.
    const Bytes big = Pattern(300 * 1024, 7);
    uint32_t idEmpty = 0, idSmall = 0, idBig = 0;
    CHECK(BleLink_Send(link, "central-A", nullptr, 0, &idEmpty) == kOk);
    CHECK(BleLink_Send(link, "central-A", reinterpret_cast<const uint8_t*>("hello"), 5, &idSmall) == kOk);
    CHECK(BleLink_Send(link, "central-A", big.data(), static_cast<int>(big.size()), &idBig) == kOk);
    CHECK(idEmpty != 0 && idSmall != 0 && idBig != 0 && idEmpty != idSmall && idSmall != idBig);
    CHECK(BleLink_Send(link, "", &one, 1, nullptr) == kInvalid);
    CHECK(BleLink_Send(link, nullptr, &one, 1, nullptr) == kInvalid);
    CHECK(BleLink_Send(link, "central-A", nullptr, 1, nullptr) == kInvalid);
    CHECK(BleLink_Send(link, "central-A", &one, -1, nullptr) == kInvalid);

    auto sentCount = [&] {
        int n = 0;
        for (auto& e : ev) n += e.ev.type == BLELINK_EVENT_MESSAGE_SENT;
        return n;
    };
    CHECK(RunUntil(link, {&c}, ev, [&] { return sentCount() == 3; }));
    std::vector<Bytes> got;
    for (auto& e : c.events) if (e.type == Link::EventType::MessageReceived) got.push_back(e.data);
    CHECK(got.size() == 3);
    bool sawBig = false, sawSmall = false, sawEmpty = false;
    for (auto& g : got) {
        sawBig = sawBig || g == big;
        sawSmall = sawSmall || g == Bytes{'h', 'e', 'l', 'l', 'o'};
        sawEmpty = sawEmpty || g.empty();
    }
    CHECK(sawBig && sawSmall && sawEmpty);
    bool progressSending = false, progressPeer = false;
    for (auto& e : ev) {
        if (e.ev.type == BLELINK_EVENT_MESSAGE_SENT) CHECK(e.device() == "central-A");
        if (e.ev.type == BLELINK_EVENT_PROGRESS && e.ev.messageId == idBig) {
            CHECK(e.ev.total == big.size());
            progressSending = progressSending || e.ev.progressKind == BLELINK_PROGRESS_SENDING;
            progressPeer = progressPeer || e.ev.progressKind == BLELINK_PROGRESS_PEER_RECEIVED;
        }
    }
    CHECK(progressSending && progressPeer);

    // Central -> server: 200 KB and a 1-byte message.
    const Bytes up = Pattern(200 * 1024, 9);
    CHECK(c.Engine().Send(Central::kServer, up).code == Link::SendOutcome::Code::Ok);
    CHECK(c.Engine().Send(Central::kServer, Bytes{42}).code == Link::SendOutcome::Code::Ok);
    auto recvCount = [&] {
        int n = 0;
        for (auto& e : ev) n += e.ev.type == BLELINK_EVENT_MESSAGE_RECEIVED;
        return n;
    };
    CHECK(RunUntil(link, {&c}, ev, [&] { return recvCount() == 2; }));
    bool upOk = false, oneOk = false, recvProgress = false;
    for (auto& e : ev) {
        if (e.ev.type == BLELINK_EVENT_MESSAGE_RECEIVED) {
            CHECK(e.ev.messageId == 0);
            CHECK(e.ev.dataLength == static_cast<int>(e.data.size()));
            upOk = upOk || e.data == up;
            oneOk = oneOk || e.data == Bytes{42};
        }
        if (e.ev.type == BLELINK_EVENT_PROGRESS && e.ev.progressKind == BLELINK_PROGRESS_RECEIVING) recvProgress = true;
    }
    CHECK(upOk && oneOk && recvProgress);
    CHECK(RunUntil(link, {&c}, ev, [&] {
        int n = 0;
        for (auto& e : c.events) n += e.type == Link::EventType::MessageSent;
        return n == 2;
    }));

    BleLink_Destroy(link);
    BleServer_Destroy(s);
}

static void TestPollBufferTooSmall() {
    void* s = MakeServer();
    void* link = BleLink_Create(s, nullptr);
    FakeBleServer* f = FakeBleServer::Last();
    Central c(f, "dev-B");
    std::vector<ServerEvent> ev;
    c.Connect();
    // The central sends below: wait until its side is READY too, not only ours.
    CHECK(RunUntil(link, {&c}, ev, [&] {
        return FindEv(ev, BLELINK_EVENT_PEER_READY) != nullptr && c.Has(Link::EventType::PeerReady);
    }));
    PollAll(link, ev);
    const Bytes msg = Pattern(1000, 3);
    CHECK(c.Engine().Send(Central::kServer, msg).code == Link::SendOutcome::Code::Ok);
    // Wait for the MESSAGE_RECEIVED event without taking it.
    bool queued = RunUntil(nullptr, {&c}, ev, [&] {
        int size = BleLink_GetEventPayloadSize(link);
        while (size == 0) {  // skip zero-payload events (progress etc.)
            BleLinkEventC e{};
            e.size = sizeof(e);
            CHECK(BleLink_PollEvent(link, &e, nullptr, 0) == 1);
            size = BleLink_GetEventPayloadSize(link);
        }
        return size == 1000;
    });
    CHECK(queued);
    BleLinkEventC e{};
    e.size = sizeof(e) - 4;
    uint8_t small[10];
    CHECK(BleLink_PollEvent(link, &e, small, 10) == -kInvalid);  // struct too small
    e.size = sizeof(e);
    CHECK(BleLink_PollEvent(link, &e, nullptr, 10) == -kInvalid);  // null buffer with length
    CHECK(BleLink_PollEvent(link, &e, small, -1) == -kInvalid);
    CHECK(BleLink_PollEvent(link, &e, small, 10) == -kTooSmall);
    CHECK(e.type == BLELINK_EVENT_MESSAGE_RECEIVED && e.dataLength == 1000);
    CHECK(std::string(e.deviceId) == "dev-B" && e.deviceIdLength == 5);
    CHECK(BleLink_GetEventPayloadSize(link) == 1000);  // still queued
    Bytes buf(1000);
    BleLinkEventC e2{};
    e2.size = sizeof(e2) + 16;  // a larger (newer) caller struct is fine
    CHECK(BleLink_PollEvent(link, &e2, buf.data(), 1000) == 1);
    CHECK(buf == msg && e2.size == sizeof(e2) + 16);
    BleLink_Destroy(link);
    BleServer_Destroy(s);
}

static void TestFlowControlAndMtu() {
    void* s = MakeServer();
    void* link = BleLink_Create(s, nullptr);
    FakeBleServer* f = FakeBleServer::Last();
    Central c(f, "dev-C");
    std::vector<ServerEvent> ev;
    c.Connect(23);  // no MTU exchange: 20-byte packets
    CHECK(RunUntil(link, {&c}, ev, [&] { return FindEv(ev, BLELINK_EVENT_PEER_READY) != nullptr; }));
    const ServerEvent* r = FindEv(ev, BLELINK_EVENT_PEER_READY);
    CHECK(r && r->ev.info.txPacketSize == 20);

    // The central's queue holds 3 notifications at a time; it frees them only on SimulatePeerReady.
    f->SetPeerTransmitCapacity("dev-C", 3);
    const Bytes msg = Pattern(5000, 11);
    uint32_t id = 0;
    CHECK(BleLink_Send(link, "dev-C", msg.data(), static_cast<int>(msg.size()), &id) == kOk);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!FindEv(ev, BLELINK_EVENT_MESSAGE_SENT) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        f->SimulatePeerReady("dev-C", 3);
        c.Drain();
        c.Pump();
        PollAll(link, ev);
    }
    CHECK(FindEv(ev, BLELINK_EVENT_MESSAGE_SENT) != nullptr);
    CHECK(f->PeerBusyCount() > 0);
    const Link::Event* got = c.Find(Link::EventType::MessageReceived);
    CHECK(got && got->data == msg);
    // Every notification respected the 20-byte limit.
    bool sizesOk = true;
    for (auto& n : f->PeerNotifyLog()) sizesOk = sizesOk && n.value.size() <= 20;
    CHECK(sizesOk);
    f->SetPeerTransmitCapacity("dev-C", -1);

    // MTU grows mid-session: later packets use it (no reset).
    f->SimulateMtu("dev-C", 185);
    BleLinkPeerInfoC info{};
    info.size = sizeof(info);
    CHECK(RunUntil(link, {&c}, ev, [&] {
        return BleLink_GetPeerInfo(link, "dev-C", &info) == kOk && info.txPacketSize == 182;
    }));
    BleLink_Destroy(link);
    BleServer_Destroy(s);
}

static void TestDisconnectAndCancel() {
    void* s = MakeServer();
    void* link = BleLink_Create(s, nullptr);
    FakeBleServer* f = FakeBleServer::Last();
    Central a(f, "dev-A"), b(f, "dev-B");
    std::vector<ServerEvent> ev;
    a.Connect();
    b.Connect(100);
    CHECK(RunUntil(link, {&a, &b}, ev, [&] {
        return FindEv(ev, BLELINK_EVENT_PEER_READY, "dev-A") && FindEv(ev, BLELINK_EVENT_PEER_READY, "dev-B");
    }));
    CHECK(BleLink_GetPeerCount(link) == 2);

    // Stall dev-A so its message stays in flight, then disconnect it.
    f->SetPeerTransmitCapacity("dev-A", 0);
    const Bytes msg = Pattern(10000, 5);
    uint32_t idA = 0, idB = 0, idCancel = 0;
    CHECK(BleLink_Send(link, "dev-A", msg.data(), static_cast<int>(msg.size()), &idA) == kOk);
    f->SetPeerTransmitCapacity("dev-B", 0);
    CHECK(BleLink_Send(link, "dev-B", msg.data(), static_cast<int>(msg.size()), &idCancel) == kOk);
    CHECK(BleLink_Cancel(link, idCancel) == kOk);
    CHECK(BleLink_Cancel(link, 0) == kInvalid);
    CHECK(BleLink_Cancel(link, 999999) == kOk);  // unknown id: ignored
    auto failed = [&](uint32_t id, int reason) {
        for (auto& e : ev)
            if (e.ev.type == BLELINK_EVENT_MESSAGE_FAILED && e.ev.messageId == id && e.ev.reason == reason) return true;
        return false;
    };
    CHECK(RunUntil(link, {&a, &b}, ev, [&] { return failed(idCancel, BLELINK_REASON_CANCELLED); }));
    f->SetPeerTransmitCapacity("dev-B", -1);
    f->SimulatePeerReady("dev-B");

    f->SimulateConnection("dev-A", false);
    CHECK(RunUntil(link, {&b}, ev, [&] {
        return failed(idA, BLELINK_REASON_DISCONNECTED) && FindEv(ev, BLELINK_EVENT_PEER_GONE, "dev-A");
    }));
    const ServerEvent* gone = FindEv(ev, BLELINK_EVENT_PEER_GONE, "dev-A");
    CHECK(gone && gone->ev.reason == BLELINK_REASON_DISCONNECTED);
    CHECK(BleLink_Send(link, "dev-A", msg.data(), 10, nullptr) == kNotReady);
    CHECK(RunUntil(link, {&b}, ev, [&] { return BleLink_GetPeerCount(link) == 1; }));

    // dev-B is unaffected.
    CHECK(BleLink_Send(link, "dev-B", msg.data(), 100, &idB) == kOk);
    CHECK(RunUntil(link, {&b}, ev, [&] {
        for (auto& e : ev) if (e.ev.type == BLELINK_EVENT_MESSAGE_SENT && e.ev.messageId == idB) return true;
        return false;
    }));

    // Unsubscribe from TX while connected: session closes, re-subscribe opens a new one.
    f->SimulateSubscription(BLELINK_SERVICE_UUID, BLELINK_TX_UUID, "dev-B", false);
    CHECK(RunUntil(link, {&b}, ev, [&] { return FindEv(ev, BLELINK_EVENT_PEER_GONE, "dev-B") != nullptr; }));
    // Unsubscribing an unrelated characteristic must not end anything (macOS reports it as a disconnect).
    BleLink_Destroy(link);
    BleServer_Destroy(s);
}

static void TestUnrelatedUnsubscribeKeepsSession() {
    void* s = MakeServer();
    CHECK(BleServer_AddService(s, "180F", true) == kOk);
    CHECK(BleServer_AddCharacteristic(s, "180F", "2A19", 0x12, 0x01) == kOk);
    void* link = BleLink_Create(s, nullptr);
    FakeBleServer* f = FakeBleServer::Last();
    Central c(f, "dev-M");
    std::vector<ServerEvent> ev;
    c.Connect();
    f->SimulateSubscription("180F", "2A19", "dev-M", true);
    CHECK(RunUntil(link, {&c}, ev, [&] { return FindEv(ev, BLELINK_EVENT_PEER_READY) != nullptr; }));
    f->SimulateSubscription("180F", "2A19", "dev-M", false);  // fires connection(false), like macOS
    const Bytes msg = Pattern(3000, 1);
    uint32_t id = 0;
    CHECK(RunUntil(link, {&c}, ev, [&] { return true; }, 50));
    CHECK(BleLink_Send(link, "dev-M", msg.data(), static_cast<int>(msg.size()), &id) == kOk);
    CHECK(RunUntil(link, {&c}, ev, [&] { return FindEv(ev, BLELINK_EVENT_MESSAGE_SENT) != nullptr; }));
    CHECK(FindEv(ev, BLELINK_EVENT_PEER_GONE) == nullptr);
    BleLink_Destroy(link);
    BleServer_Destroy(s);
}

static void TestOwnMaxMessageAdvertised() {
    // Our maxMessage reaches the central through HELLO: it refuses to send 101 bytes.
    void* s = MakeServer();
    FakeBleServer* f = FakeBleServer::Last();
    BleLinkConfigC small{};
    small.size = sizeof(small);
    small.maxMessage = 100;
    void* link2 = BleLink_Create(s, &small);
    CHECK(link2 != nullptr);
    Central c2(f, "dev-T");
    std::vector<ServerEvent> ev2;
    c2.Connect();
    CHECK(RunUntil(link2, {&c2}, ev2, [&] { return c2.Has(Link::EventType::PeerReady); }));
    CHECK(c2.Engine().Send(Central::kServer, Pattern(101, 1)).code == Link::SendOutcome::Code::TooLarge);
    BleLink_Destroy(link2);
    BleServer_Destroy(s);
}

static void TestTooLargeToPeer() {
    // The central advertises maxMessage 1000: BleLink_Send of 1001 bytes -> MessageTooLarge.
    void* s = MakeServer();
    void* link = BleLink_Create(s, nullptr);
    FakeBleServer* f = FakeBleServer::Last();
    class SmallCentral : public Link::ITransport {
    public:
        explicit SmallCentral(FakeBleServer* fk) : f_(fk), e_(*this, Cfg()) {}
        static Link::Config Cfg() {
            Link::Config c;
            c.role = Link::Role::Client;
            c.maxMessage = 1000;
            c.window = 1;
            return c;
        }
        Link::TransportResult SendPacket(const Link::PeerId&, const uint8_t* d, size_t n) override {
            f_->SimulateWriteEx(BLELINK_SERVICE_UUID, BLELINK_RX_UUID, "mcu", Bytes(d, d + n), false);
            return Link::TransportResult::Sent;
        }
        FakeBleServer* f_;
        Link::Engine e_;
    } mcu(f);
    f->SimulateConnection("mcu", true);
    f->SimulateSubscription(BLELINK_SERVICE_UUID, BLELINK_TX_UUID, "mcu", true);
    mcu.e_.OnPeerSubscribed("server");
    mcu.e_.Pump();
    std::vector<ServerEvent> ev;
    CHECK(RunUntil(link, {}, ev, [&] { return FindEv(ev, BLELINK_EVENT_PEER_READY) != nullptr; }));
    const ServerEvent* r = FindEv(ev, BLELINK_EVENT_PEER_READY);
    CHECK(r && r->ev.info.peerMaxMessage == 1000 && r->ev.info.peerWindow == 1);
    const Bytes m = Pattern(1001, 2);
    CHECK(BleLink_Send(link, "mcu", m.data(), 1001, nullptr) == kTooLarge);
    CHECK(BleLink_Send(link, "mcu", m.data(), 1000, nullptr) == kOk);
    BleLink_Destroy(link);
    BleServer_Destroy(s);
}

static void TestServerShutdownAndDestroy() {
    void* s = MakeServer();
    void* link = BleLink_Create(s, nullptr);
    FakeBleServer* f = FakeBleServer::Last();
    Central c(f, "dev-Z");
    std::vector<ServerEvent> ev;
    c.Connect();
    CHECK(RunUntil(link, {&c}, ev, [&] { return FindEv(ev, BLELINK_EVENT_PEER_READY) != nullptr; }));
    f->SetPeerTransmitCapacity("dev-Z", 0);  // keep the message pending
    const Bytes msg = Pattern(4000, 4);
    uint32_t id = 0;
    CHECK(BleLink_Send(link, "dev-Z", msg.data(), 4000, &id) == kOk);
    CHECK(RunUntil(link, {&c}, ev, [&] { return true; }, 30));
    BleServer_Shutdown(s);
    // The failure is still pollable after Shutdown.
    PollAll(link, ev);
    bool failedShutdown = false;
    for (auto& e : ev)
        failedShutdown = failedShutdown ||
            (e.ev.type == BLELINK_EVENT_MESSAGE_FAILED && e.ev.messageId == id && e.ev.reason == BLELINK_REASON_SHUTDOWN);
    CHECK(failedShutdown);
    CHECK(BleLink_Send(link, "dev-Z", msg.data(), 10, nullptr) == kNotInit);
    BleLinkPeerInfoC info{};
    info.size = sizeof(info);
    BleLink_GetPeerInfo(link, "dev-Z", &info);  // any result, must not crash
    CHECK(BleLink_GetEventPayloadSize(link) == -1);
    // Re-initialize: the old link stays dead; a new one can be created.
    CHECK(BleServer_Initialize(s, "again", false) == kOk);
    CHECK(BleLink_Send(link, "dev-Z", msg.data(), 10, nullptr) == kNotInit);
    void* link2 = BleLink_Create(s, nullptr);
    CHECK(link2 != nullptr);
    BleLink_Destroy(link);  // destroying the dead link must not remove link2's service
    auto services = FakeBleServer::Last()->GetServices();
    bool found = false;
    for (auto& sv : services) found = found || sv == BLELINK_SERVICE_UUID;
    CHECK(found);

    // Destroy the server first, then use and destroy the link.
    BleServer_Destroy(s);
    CHECK(BleLink_Send(link2, "x", msg.data(), 1, nullptr) == kNotInit);
    CHECK(BleLink_GetPeerCount(link2) == 0);
    BleLink_Destroy(link2);
    CHECK(BleServer::Internal::LiveLinkHandles() == 0);
}

static void TestConcurrentSendAndPoll() {
    void* s = MakeServer();
    void* link = BleLink_Create(s, nullptr);
    FakeBleServer* f = FakeBleServer::Last();
    Central c(f, "dev-T");
    std::vector<ServerEvent> ev;
    c.Connect();
    CHECK(RunUntil(link, {&c}, ev, [&] { return FindEv(ev, BLELINK_EVENT_PEER_READY) != nullptr; }));
    std::atomic<int> sent{0};
    std::atomic<bool> stop{false};
    auto sender = [&](uint32_t seed) {
        for (int i = 0; i < 40; ++i) {
            const Bytes m = Pattern(100 + i * 37, seed + i);
            if (BleLink_Send(link, "dev-T", m.data(), static_cast<int>(m.size()), nullptr) == kOk) ++sent;
        }
    };
    std::thread t1(sender, 100), t2(sender, 200);
    // A third thread hammers read-only entry points.
    std::thread t3([&] {
        BleLinkPeerInfoC info{};
        info.size = sizeof(info);
        while (!stop.load()) {
            BleLink_GetPeerInfo(link, "dev-T", &info);
            BleLink_GetPeerCount(link);
        }
    });
    t1.join();
    t2.join();
    CHECK(sent.load() == 80);
    auto sentEvents = [&] {
        int n = 0;
        for (auto& e : ev) n += e.ev.type == BLELINK_EVENT_MESSAGE_SENT;
        return n;
    };
    CHECK(RunUntil(link, {&c}, ev, [&] { return sentEvents() == 80; }, 60000));
    int received = 0;
    for (auto& e : c.events) received += e.type == Link::EventType::MessageReceived;
    CHECK(received == 80);
    // Destroy the link while another thread is still calling into it.
    BleLink_Destroy(link);
    stop.store(true);
    t3.join();
    BleServer_Destroy(s);
}

int main() {
    TestVersionAndLayout();
    TestInvalidHandles();
    TestCreateValidation();
    TestRoundTrip();
    TestPollBufferTooSmall();
    TestFlowControlAndMtu();
    TestDisconnectAndCancel();
    TestUnrelatedUnsubscribeKeepsSession();
    TestOwnMaxMessageAdvertised();
    TestTooLargeToPeer();
    TestServerShutdownAndDestroy();
    TestConcurrentSendAndPoll();
    CHECK(BleServer::Internal::LiveLinkHandles() == 0);
    std::printf("blelink_abi: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
