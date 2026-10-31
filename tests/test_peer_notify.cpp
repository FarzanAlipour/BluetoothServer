// Per-central transport (IBleServer::NotifyPeer, busy/ready, per-central subscriptions and max
// payload, ordered writes): the shared helpers in src/common (PeerSendGate, OrderedDelivery) and the
// fake backend's model of them, which the BleLink engine tests drive. No hardware.
#include "IBleServer.h"
#include "FakeBleServer.h"
#include "OrderedDelivery.h"
#include "PeerSendGate.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace BleServer;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

using Bytes = std::vector<uint8_t>;
static const char* kSvc = "7b1e1000-5a6c-4f0e-9d3b-2c1a8e4f6b01";
static const char* kTx = "7b1e1002-5a6c-4f0e-9d3b-2c1a8e4f6b01";
static const char* kRx = "7b1e1001-5a6c-4f0e-9d3b-2c1a8e4f6b01";

// ---------------------------------------------------------------- PeerSendGate
static void TestGateInFlightLimit() {
    PeerSendGate g(1);
    CHECK(g.TryAcquire("A"));
    CHECK(!g.TryAcquire("A"));  // second in flight -> busy, A now waiting
    CHECK(g.IsWaiting("A"));
    CHECK(g.TryAcquire("B"));   // other peers are independent
    CHECK(g.InFlight("A") == 1 && g.InFlight("B") == 1);
    CHECK(g.Release("A"));      // A was waiting: owner must signal ready
    CHECK(!g.IsWaiting("A") && g.InFlight("A") == 0);
    CHECK(!g.Release("B"));     // B never waited: no signal
    CHECK(g.TryAcquire("A"));
    g.Cancel("A");
    CHECK(g.InFlight("A") == 0);
}

static void TestGateWaitingFlags() {
    PeerSendGate g;
    g.MarkWaiting("A");
    g.MarkWaiting("B");
    g.MarkWaiting("A");
    CHECK(g.AnyWaiting());
    CHECK(g.TakeWaiting("A"));
    CHECK(!g.TakeWaiting("A"));
    auto all = g.TakeAllWaiting();
    CHECK(all.size() == 1 && all[0] == "B");
    CHECK(!g.AnyWaiting());
    g.MarkWaiting("C");
    g.Forget("C");
    CHECK(!g.IsWaiting("C"));
    PeerSendGate g2(2);
    CHECK(g2.TryAcquire("A") && g2.TryAcquire("A") && !g2.TryAcquire("A"));
    g2.Clear();
    CHECK(g2.InFlight("A") == 0 && !g2.AnyWaiting());
}

// ---------------------------------------------------------------- OrderedDelivery
static void TestOrderedDeliveryOutOfOrder() {
    OrderedDelivery<int> d;
    std::vector<int> out;
    auto deliver = [&](int v) { out.push_back(v); };
    const uint64_t t0 = d.Reserve(), t1 = d.Reserve(), t2 = d.Reserve(), t3 = d.Reserve();
    d.Complete(t2, 2, deliver);
    d.Complete(t1, 1, deliver);
    CHECK(out.empty());          // t0 still pending: nothing may overtake it
    CHECK(d.Pending() == 4);
    d.Complete(t0, 0, deliver);
    CHECK((out == std::vector<int>{0, 1, 2}));
    d.Complete(t3, std::nullopt, deliver);  // skipped (e.g. GetRequestAsync failed)
    CHECK(out.size() == 3 && d.Pending() == 0);
    d.Complete(t3, 9, deliver);  // stale ticket: ignored
    d.Complete(77, 9, deliver);  // never reserved: ignored
    CHECK(out.size() == 3);
    const uint64_t t4 = d.Reserve();
    d.Reset();                   // shutdown: t4 is skipped
    d.Complete(t4, 4, deliver);
    const uint64_t t5 = d.Reserve();
    d.Complete(t5, 5, deliver);
    CHECK((out == std::vector<int>{0, 1, 2, 5}));
}

static void TestOrderedDeliveryThreads() {
    // Writes reserved in order, completed on many threads in random order: delivered in order, never
    // two deliveries at once.
    OrderedDelivery<int> d;
    constexpr int kN = 400;
    std::vector<uint64_t> tickets;
    for (int i = 0; i < kN; ++i) tickets.push_back(d.Reserve());
    std::vector<int> order(kN);
    for (int i = 0; i < kN; ++i) order[i] = i;
    std::mt19937 rng(1234);
    std::shuffle(order.begin(), order.end(), rng);
    std::vector<int> out;
    std::atomic<int> active{0};
    std::atomic<bool> overlap{false};
    auto deliver = [&](int v) {
        if (active.fetch_add(1) != 0) overlap = true;
        out.push_back(v);
        active.fetch_sub(1);
    };
    std::vector<std::thread> threads;
    std::atomic<int> next{0};
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&] {
            for (int i; (i = next.fetch_add(1)) < kN;) d.Complete(tickets[order[i]], order[i], deliver);
        });
    for (auto& t : threads) t.join();
    CHECK(!overlap.load());
    CHECK(static_cast<int>(out.size()) == kN);
    bool inOrder = true;
    for (int i = 0; i < static_cast<int>(out.size()); ++i) inOrder = inOrder && out[i] == i;
    CHECK(inOrder);
}

// ---------------------------------------------------------------- Fake backend model
struct Recorder {
    std::mutex m;
    std::vector<std::string> ready;                                   // ReadyToSend device ids
    std::vector<std::tuple<std::string, std::string, bool>> subs;     // (chr, device, subscribed)
    std::map<std::string, size_t> maxPayload;
    std::vector<std::pair<std::string, Bytes>> writes;
};

static void Setup(FakeBleServer& srv, Recorder& rec) {
    CHECK(srv.Initialize(ServerConfig{}) == BleError::Success);
    CHECK(srv.AddService(ServiceConfig{kSvc, true}) == BleError::Success);
    CharacteristicConfig tx;
    tx.uuid = kTx;
    tx.properties = static_cast<uint32_t>(CharacteristicProperty::Notify);
    CHECK(srv.AddCharacteristic(kSvc, tx) == BleError::Success);
    CharacteristicConfig rx;
    rx.uuid = kRx;
    rx.properties = static_cast<uint32_t>(CharacteristicProperty::Write) |
                    static_cast<uint32_t>(CharacteristicProperty::WriteWithoutResponse);
    CHECK(srv.AddCharacteristic(kSvc, rx) == BleError::Success);
    srv.SetReadyToSendCallback([&rec](const std::string& d) { std::lock_guard<std::mutex> l(rec.m); rec.ready.push_back(d); });
    srv.SetSubscriptionCallback([&rec](const std::string&, const std::string& c, const std::string& d, bool s) {
        std::lock_guard<std::mutex> l(rec.m);
        rec.subs.emplace_back(c, d, s);
    });
    srv.SetMaxPayloadCallback([&rec](const std::string& d, size_t n) { std::lock_guard<std::mutex> l(rec.m); rec.maxPayload[d] = n; });
    srv.SetCharacteristicWriteCallback(kSvc, kRx, [&rec](const std::string& d, const Bytes& b) {
        std::lock_guard<std::mutex> l(rec.m);
        rec.writes.emplace_back(d, b);
    });
}

static NotifyStatus Send(FakeBleServer& srv, const std::string& dev, uint8_t v, size_t len = 1) {
    Bytes b(len, v);
    return srv.NotifyPeer(kSvc, kTx, dev, b.data(), b.size());
}

static void TestFakeSubscriptionAndTargeting() {
    FakeBleServer srv;
    Recorder rec;
    Setup(srv, rec);
    CHECK((srv.GetCapabilities() & static_cast<uint32_t>(ServerCapability::PeerNotify)) != 0);
    CHECK(Send(srv, "A", 1) == NotifyStatus::NotSubscribed);
    srv.SimulateSubscription(kSvc, kTx, "A", true);
    srv.SimulateSubscription(kSvc, kTx, "B", true);
    srv.SimulateSubscription(kSvc, kTx, "B", true);  // duplicate: no second event
    CHECK(rec.subs.size() == 2);
    CHECK(srv.GetSubscribers(kSvc, kTx).size() == 2);
    CHECK(Send(srv, "A", 1) == NotifyStatus::Sent);
    CHECK(Send(srv, "B", 2) == NotifyStatus::Sent);
    CHECK(Send(srv, "A", 3) == NotifyStatus::Sent);
    auto log = srv.PeerNotifyLog();
    CHECK(log.size() == 3);
    CHECK(log[0].device == "A" && log[0].value == Bytes{1});
    CHECK(log[1].device == "B" && log[1].value == Bytes{2});
    CHECK(log[2].device == "A" && log[2].value == Bytes{3});
    // NotifyPeer does not touch the stored value (unlike UpdateCharacteristicValue).
    CHECK(srv.LastValue(kSvc, kTx).empty());
    // Bad arguments.
    CHECK(srv.NotifyPeer(kSvc, "nope", "A", nullptr, 0) == NotifyStatus::Error);
    CHECK(srv.NotifyPeer(kSvc, kTx, "", nullptr, 0) == NotifyStatus::Error);
    CHECK(srv.NotifyPeer(kSvc, kTx, "A", nullptr, 4) == NotifyStatus::Error);
    // Unsubscribe.
    srv.SimulateSubscription(kSvc, kTx, "A", false);
    CHECK(std::get<2>(rec.subs.back()) == false && std::get<1>(rec.subs.back()) == "A");
    CHECK(Send(srv, "A", 4) == NotifyStatus::NotSubscribed);
    CHECK(Send(srv, "B", 5) == NotifyStatus::Sent);
    // Base-class default for backends without the capability.
    CHECK(srv.IBleServer::NotifyPeer(kSvc, kTx, "B", nullptr, 0) == NotifyStatus::NotSupported);
    CHECK(srv.IBleServer::GetMaxNotifyPayload("B") == 0);
}

static void TestFakePerPeerBusyReady() {
    FakeBleServer srv;
    Recorder rec;
    Setup(srv, rec);
    srv.SimulateSubscription(kSvc, kTx, "A", true);
    srv.SimulateSubscription(kSvc, kTx, "B", true);
    srv.SetPeerTransmitCapacity("A", 2);
    CHECK(Send(srv, "A", 1) == NotifyStatus::Sent);
    CHECK(Send(srv, "A", 2) == NotifyStatus::Sent);
    CHECK(Send(srv, "A", 3) == NotifyStatus::Busy);   // nothing sent
    CHECK(Send(srv, "B", 9) == NotifyStatus::Sent);   // B unaffected (Windows/Linux: per central)
    CHECK(srv.PeerNotifyLog().size() == 3);
    CHECK(srv.IsPeerWaiting("A") && !srv.IsPeerWaiting("B"));
    CHECK(!srv.SimulatePeerReady("B"));               // B never waited: no signal
    CHECK(rec.ready.empty());
    CHECK(srv.SimulatePeerReady("A", 1));
    CHECK(rec.ready.size() == 1 && rec.ready[0] == "A");
    CHECK(Send(srv, "A", 3) == NotifyStatus::Sent);   // retry of the same value
    CHECK(Send(srv, "A", 4) == NotifyStatus::Busy);
    CHECK(srv.PeerBusyCount() == 2);
    // A disconnect ends the wait: no ready owed, subscriptions gone, unsubscribe reported.
    const size_t subsBefore = rec.subs.size();
    srv.SimulateConnection("A", false);
    CHECK(rec.subs.size() == subsBefore + 1);
    CHECK(!srv.IsPeerWaiting("A"));
    CHECK(!srv.SimulatePeerReady("A"));
    CHECK(Send(srv, "A", 5) == NotifyStatus::NotSubscribed);
}

static void TestFakeSharedQueueReady() {
    // macOS model: one transmit queue for every central, one "ready" for all.
    FakeBleServer srv;
    Recorder rec;
    Setup(srv, rec);
    srv.SimulateSubscription(kSvc, kTx, "A", true);
    srv.SimulateSubscription(kSvc, kTx, "B", true);
    srv.SetTransmitCapacity(1);
    CHECK(Send(srv, "A", 1) == NotifyStatus::Sent);
    CHECK(Send(srv, "A", 2) == NotifyStatus::Busy);
    CHECK(Send(srv, "B", 3) == NotifyStatus::Busy);
    srv.SimulateReadyToUpdate();
    CHECK(rec.ready.size() == 1 && rec.ready[0].empty());  // "" = every central
    CHECK(!srv.IsPeerWaiting("A") && !srv.IsPeerWaiting("B"));
    srv.SimulateReadyToUpdate();                            // nobody waiting: no signal
    CHECK(rec.ready.size() == 1);
    // Legacy queued values are served first: NotifyPeer is Busy while they wait.
    srv.SetTransmitCapacity(0);
    CHECK(srv.NotifyCharacteristicChanged(kSvc, kTx) == BleError::Success);
    CHECK(srv.PendingNotifications() == 1);
    srv.SetTransmitCapacity(-1);
    CHECK(Send(srv, "A", 4) == NotifyStatus::Busy);
    CHECK(srv.SimulateReadyToUpdate() == 1);
    CHECK(rec.ready.size() == 2);
    CHECK(Send(srv, "A", 4) == NotifyStatus::Sent);
}

static void TestFakeMaxPayload() {
    FakeBleServer srv;
    Recorder rec;
    Setup(srv, rec);
    srv.SimulateSubscription(kSvc, kTx, "A", true);
    CHECK(srv.GetMaxNotifyPayload("A") == 0);  // unknown
    CHECK(Send(srv, "A", 1, 300) == NotifyStatus::Sent);  // unknown limit: the OS decides
    srv.SimulateMtu("A", 247);
    CHECK(srv.GetMaxNotifyPayload("A") == 244);
    CHECK(rec.maxPayload["A"] == 244);
    CHECK(Send(srv, "A", 1, 244) == NotifyStatus::Sent);
    CHECK(Send(srv, "A", 1, 245) == NotifyStatus::TooLarge);
    rec.maxPayload.clear();
    srv.SimulateMtu("A", 247);                 // unchanged: no callback
    CHECK(rec.maxPayload.empty());
    srv.SimulateMtu("A", 517);                 // renegotiated
    CHECK(rec.maxPayload["A"] == 514);
    srv.SimulateMtu("A", 3);                   // nonsense: ignored
    CHECK(srv.GetMaxNotifyPayload("A") == 514);
    srv.SimulateConnection("A", false);
    CHECK(srv.GetMaxNotifyPayload("A") == 0);
}

static void TestFakeErrorsAndShutdown() {
    FakeBleServer srv;
    Recorder rec;
    Setup(srv, rec);
    srv.SimulateSubscription(kSvc, kTx, "A", true);
    srv.FailNextPeerNotifies(1);
    CHECK(Send(srv, "A", 1) == NotifyStatus::Error);
    CHECK(Send(srv, "A", 1) == NotifyStatus::Sent);
    srv.SetPeerTransmitCapacity("A", 0);
    CHECK(Send(srv, "A", 2) == NotifyStatus::Busy);
    srv.Shutdown();
    CHECK(Send(srv, "A", 2) == NotifyStatus::Error);
    CHECK(!srv.SimulatePeerReady("A"));  // shutdown dropped the owed signal
    CHECK(rec.ready.empty());
}

static void TestFakeOrderedWrites() {
    FakeBleServer srv;
    Recorder rec;
    Setup(srv, rec);
    for (uint8_t i = 0; i < 50; ++i)
        CHECK(srv.SimulateWriteEx(kSvc, kRx, (i % 2) ? "A" : "B", Bytes{i}, (i % 3) == 0));
    CHECK(rec.writes.size() == 50);
    bool ok = true;
    for (uint8_t i = 0; i < 50; ++i)
        ok = ok && rec.writes[i].first == ((i % 2) ? "A" : "B") && rec.writes[i].second == Bytes{i};
    CHECK(ok);
    auto log = srv.WriteLog();
    CHECK(log.size() == 50 && log[0].withResponse && !log[1].withResponse);
}

// An engine-like sender: per peer a list of packets; send until Busy, resume on ReadyToSend. The
// ready callback calls NotifyPeer re-entrantly (allowed by the contract). Everything must arrive,
// in order, per peer, with no loss.
static void TestFakeFlowControlledBurst() {
    FakeBleServer srv;
    Recorder rec;
    Setup(srv, rec);
    const std::vector<std::string> peers = {"A", "B", "C"};
    for (auto& p : peers) srv.SimulateSubscription(kSvc, kTx, p, true);
    constexpr int kPackets = 300;
    std::map<std::string, int> next;
    std::function<void(const std::string&)> pump = [&](const std::string& only) {
        for (auto& p : peers) {
            if (!only.empty() && p != only) continue;
            while (next[p] < kPackets) {
                uint8_t v = static_cast<uint8_t>(next[p] & 0xFF);
                NotifyStatus s = Send(srv, p, v, 1);
                if (s == NotifyStatus::Busy) break;
                CHECK(s == NotifyStatus::Sent);
                ++next[p];
            }
        }
    };
    srv.SetReadyToSendCallback([&](const std::string& d) { pump(d); });
    srv.SetPeerTransmitCapacity("A", 7);
    srv.SetPeerTransmitCapacity("B", 3);
    srv.SetPeerTransmitCapacity("C", 11);
    pump("");
    for (int round = 0; round < 1000; ++round) {
        bool done = true;
        for (auto& p : peers) done = done && next[p] == kPackets;
        if (done) break;
        for (auto& p : peers) srv.SimulatePeerReady(p, 1 + (round % 9));
    }
    std::map<std::string, std::vector<uint8_t>> got;
    for (auto& e : srv.PeerNotifyLog()) got[e.device].push_back(e.value[0]);
    for (auto& p : peers) {
        CHECK(static_cast<int>(got[p].size()) == kPackets);
        bool inOrder = true;
        for (int i = 0; i < static_cast<int>(got[p].size()); ++i)
            inOrder = inOrder && got[p][i] == static_cast<uint8_t>(i & 0xFF);
        CHECK(inOrder);
    }
    CHECK(srv.PeerBusyCount() > 0);
}

int main() {
    TestGateInFlightLimit();
    TestGateWaitingFlags();
    TestOrderedDeliveryOutOfOrder();
    TestOrderedDeliveryThreads();
    TestFakeSubscriptionAndTargeting();
    TestFakePerPeerBusyReady();
    TestFakeSharedQueueReady();
    TestFakeMaxPayload();
    TestFakeErrorsAndShutdown();
    TestFakeOrderedWrites();
    TestFakeFlowControlledBurst();
    std::printf("peer_notify: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
