// Notification flow control: NotificationQueue (src/common) on its own, and through the C ABI against
// the fake backend, which models CoreBluetooth's "transmit queue full" / "ready to update" cycle with
// the same queue the macOS backend uses. No hardware.
#include "IBleServer.h"
#include "NotificationQueue.h"
#include "FakeBleServer.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace BleServer;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

using Bytes = std::vector<uint8_t>;
using Outcome = NotificationQueue::SendOutcome;
using Item = NotificationQueue::Item;

static Item MakeItem(uint8_t v, const char* chr = "2A19") { return Item{"180F", chr, Bytes{v}}; }

// Scripted transport: `credits` sends succeed, then Busy. Records what went out.
struct Transport {
    int credits = -1;
    std::vector<uint8_t> sent;
    Outcome operator()(const Item& it) {
        if (credits == 0) return Outcome::Busy;
        if (credits > 0) --credits;
        sent.push_back(it.value.empty() ? 0 : it.value[0]);
        return Outcome::Sent;
    }
};

// ---------------------------------------------------------------- NotificationQueue unit tests
static void TestQueueSendsImmediatelyWhenEmpty() {
    NotificationQueue q;
    Transport t;
    auto r = q.Submit(MakeItem(1), [&](const Item& i) { return t(i); });
    CHECK(r.sentNow && !r.queued && !r.backlogStarted && r.dropped == 0);
    CHECK(q.Empty());
    CHECK(t.sent == Bytes({1}));
}

static void TestQueueKeepsOrder() {
    NotificationQueue q;
    Transport t;
    t.credits = 2;
    auto send = [&](const Item& i) { return t(i); };
    CHECK(q.Submit(MakeItem(1), send).sentNow);
    CHECK(q.Submit(MakeItem(2), send).sentNow);
    auto r3 = q.Submit(MakeItem(3), send);
    CHECK(r3.queued && r3.backlogStarted && r3.depth == 1);
    // The transport has room again, but 4 must not overtake 3.
    t.credits = -1;
    auto r4 = q.Submit(MakeItem(4), send);
    CHECK(r4.queued && !r4.sentNow && !r4.backlogStarted && r4.depth == 2);
    CHECK(t.sent == Bytes({1, 2}));

    t.credits = 1;
    auto d1 = q.Drain(send);
    CHECK(d1.sent == 1 && d1.remaining == 1 && !d1.backlogCleared);
    t.credits = -1;
    auto d2 = q.Drain(send);
    CHECK(d2.sent == 1 && d2.remaining == 0 && d2.backlogCleared);
    CHECK(t.sent == Bytes({1, 2, 3, 4}));
    CHECK(q.HighWater() == 2);
    // Draining an empty queue is a no-op.
    auto d3 = q.Drain(send);
    CHECK(d3.sent == 0 && !d3.backlogCleared);
}

static void TestQueueBoundDropsOldest() {
    NotificationQueue q(3);
    Transport t;
    t.credits = 0;
    auto send = [&](const Item& i) { return t(i); };
    size_t dropped = 0;
    for (uint8_t v = 1; v <= 5; ++v) dropped += q.Submit(MakeItem(v), send).dropped;
    CHECK(dropped == 2);
    CHECK(q.Size() == 3);
    CHECK(q.TotalDropped() == 2);
    t.credits = -1;
    q.Drain(send);
    CHECK(t.sent == Bytes({3, 4, 5}));
}

static void TestQueueRemoveAndDiscard() {
    NotificationQueue q;
    Transport t;
    t.credits = 0;
    auto send = [&](const Item& i) { return t(i); };
    q.Submit(MakeItem(1, "AAAA"), send);
    q.Submit(MakeItem(2, "BBBB"), send);
    q.Submit(MakeItem(3, "AAAA"), send);
    CHECK(q.RemoveCharacteristic("180F", "AAAA") == 2);
    CHECK(q.Size() == 1);
    CHECK(q.RemoveService("180F") == 1);
    CHECK(q.Empty());

    // Discard: dropped without being sent, drain continues past it.
    q.Submit(MakeItem(4), send);
    q.Submit(MakeItem(5, "GONE"), send);
    q.Submit(MakeItem(6), send);
    auto d = q.Drain([&](const Item& i) {
        if (i.characteristic == "GONE") return Outcome::Discard;
        t.credits = -1;
        return t(i);
    });
    CHECK(d.sent == 2 && d.discarded == 1 && d.remaining == 0);
    CHECK(t.sent == Bytes({4, 6}));

    // Discard on an empty queue: nothing kept, nothing sent.
    auto r = q.Submit(MakeItem(7), [](const Item&) { return Outcome::Discard; });
    CHECK(!r.sentNow && !r.queued && q.Empty());

    q.Submit(MakeItem(8), [](const Item&) { return Outcome::Busy; });
    CHECK(q.Clear() == 1 && q.Empty());
}

// ---------------------------------------------------------------- Through the C ABI + fake backend
static const char* S = "180F";
static const char* C = "2A19";
static const char* DEV = "central-1";

struct Report { int code; std::string message; };
static std::mutex g_reportsMutex;
static std::vector<Report> g_reports;
static void* g_server = nullptr;
static int g_reentrantCalls = 0;

static void OnError(int code, const char* message) {
    {
        std::lock_guard<std::mutex> l(g_reportsMutex);
        g_reports.push_back({code, message ? message : ""});
    }
    // Re-enter the API from the callback: deadlocks if the backend still holds its lock.
    if (g_server && BleServer_GetConnectedDeviceCount(g_server) >= 0) ++g_reentrantCalls;
}

static bool Reported(int code, const char* needle) {
    std::lock_guard<std::mutex> l(g_reportsMutex);
    for (auto& r : g_reports)
        if (r.code == code && r.message.find(needle) != std::string::npos) return true;
    return false;
}

struct Rig {
    void* server = nullptr;
    FakeBleServer* fake = nullptr;
    Rig() {
        {
            std::lock_guard<std::mutex> l(g_reportsMutex);
            g_reports.clear();
        }
        g_reentrantCalls = 0;
        server = BleServer_Create();
        BleServer_Initialize(server, "T", false);
        BleServer_SetErrorCallback(server, OnError);
        fake = FakeBleServer::Last();
        BleServer_AddService(server, S, true);
        BleServer_AddCharacteristic(server, S, C, 0x12, 0x01);
        g_server = server;
    }
    ~Rig() {
        g_server = nullptr;
        BleServer_Destroy(server);
    }
    int Notify(uint8_t v) {
        uint8_t b = v;
        BleServer_UpdateCharacteristicValue(server, S, C, &b, 1);
        return BleServer_NotifyCharacteristicChanged(server, S, C, nullptr);
    }
    Bytes Sent() const {
        Bytes out;
        for (auto& e : fake->NotifyLog()) out.push_back(e.value.empty() ? 0 : e.value[0]);
        return out;
    }
};

// The `l` command case: a 5-chunk chunked message sent back to back into a small OS queue.
static void TestChunkBurstIsNotDropped() {
    Rig r;
    r.fake->SimulateSubscription(S, C, DEV, true);
    r.fake->SetTransmitCapacity(2);

    Bytes msg(243 * 4 + 10);
    for (size_t i = 0; i < msg.size(); ++i) msg[i] = static_cast<uint8_t>(i * 7 + 3);
    void* sender = BleChunker_Create();
    CHECK(BleChunker_SendMessage(sender, r.server, S, C, msg.data(), static_cast<int>(msg.size())) == 0);
    auto wire = r.fake->ValueLog();
    CHECK(wire.size() == 5);
    CHECK(r.fake->NotifyLog().size() == 2);         // what the OS took
    CHECK(r.fake->PendingNotifications() == 3);     // the rest is queued, not lost
    CHECK(Reported(0, "backed up"));

    CHECK(r.fake->SimulateReadyToUpdate(1) == 1);   // room for one more
    CHECK(r.fake->PendingNotifications() == 2);
    CHECK(r.fake->SimulateReadyToUpdate() == 2);
    CHECK(r.fake->PendingNotifications() == 0);
    CHECK(Reported(0, "drained"));

    // Every chunk went out exactly once, in order, with the value it had when notified.
    auto log = r.fake->NotifyLog();
    CHECK(log.size() == wire.size());
    for (size_t i = 0; i < log.size() && i < wire.size(); ++i) CHECK(log[i].value == wire[i]);

    // And a receiver reassembles the original message.
    void* receiver = BleChunker_Create();
    for (auto& e : log) {
        uint8_t ack[64]; int ackLen = sizeof(ack);
        BleChunker_ProcessIncoming(receiver, e.value.data(), static_cast<int>(e.value.size()), ack, &ackLen);
    }
    int n = BleChunker_GetNextMessageSize(receiver);
    CHECK(n == static_cast<int>(msg.size()));
    Bytes got(static_cast<size_t>(n > 0 ? n : 0));
    int len = n;
    CHECK(n > 0 && BleChunker_ReadNextMessage(receiver, got.data(), &len));
    CHECK(got == msg);
    BleChunker_Destroy(receiver);
    BleChunker_Destroy(sender);
    CHECK(g_reentrantCalls > 0);
}

static void TestValueCapturedAtNotifyTime() {
    Rig r;
    r.fake->SimulateSubscription(S, C, DEV, true);
    r.fake->SetTransmitCapacity(0);
    CHECK(r.Notify(10) == 0);
    CHECK(r.Notify(11) == 0);
    uint8_t later = 99;  // a later value update must not rewrite what was queued
    BleServer_UpdateCharacteristicValue(r.server, S, C, &later, 1);
    r.fake->SimulateReadyToUpdate();
    CHECK(r.Sent() == Bytes({10, 11}));
}

static void TestOverflowDropsOldestAndReports() {
    Rig r;
    r.fake->SimulateSubscription(S, C, DEV, true);
    CHECK(r.fake->SetNotifyQueueCapacity(3));
    r.fake->SetTransmitCapacity(0);
    for (uint8_t v = 1; v <= 5; ++v) CHECK(r.Notify(v) == 0);
    CHECK(r.fake->PendingNotifications() == 3);
    CHECK(Reported(static_cast<int>(BleError::Unknown), "dropped 1 oldest"));
    CHECK(!r.fake->SetNotifyQueueCapacity(10));   // not while values are queued
    r.fake->SimulateReadyToUpdate();
    CHECK(r.Sent() == Bytes({3, 4, 5}));
}

static void TestLastUnsubscribeClearsQueue() {
    Rig r;
    r.fake->SimulateSubscription(S, C, "a", true);
    r.fake->SimulateSubscription(S, C, "b", true);
    r.fake->SetTransmitCapacity(0);
    r.Notify(1);
    r.Notify(2);
    r.fake->SimulateSubscription(S, C, "a", false);
    CHECK(r.fake->PendingNotifications() == 2);   // "b" still subscribed
    r.fake->SimulateSubscription(S, C, "b", false);
    CHECK(r.fake->PendingNotifications() == 0);
    CHECK(Reported(0, "Last subscriber left"));
    r.fake->SimulateReadyToUpdate();
    CHECK(r.Sent().empty());
}

static void TestNoSubscriberIsNotQueued() {
    Rig r;
    r.fake->SetTransmitCapacity(0);
    CHECK(r.Notify(1) == 0);
    CHECK(r.fake->PendingNotifications() == 0);
}

static void TestShutdownAndRemoveClearQueue() {
    {
        Rig r;
        r.fake->SimulateSubscription(S, C, DEV, true);
        r.fake->SetTransmitCapacity(0);
        r.Notify(1);
        r.Notify(2);
        CHECK(r.fake->PendingNotifications() == 2);
        BleServer_Shutdown(r.server);
        CHECK(r.fake->PendingNotifications() == 0);
        CHECK(BleServer_NotifyCharacteristicChanged(r.server, S, C, nullptr) ==
              static_cast<int>(BleError::NotInitialized));
    }
    {
        Rig r;
        r.fake->SimulateSubscription(S, C, DEV, true);
        r.fake->SetTransmitCapacity(0);
        r.Notify(1);
        CHECK(BleServer_RemoveCharacteristic(r.server, S, C) == 0);
        CHECK(r.fake->PendingNotifications() == 0);
    }
    {
        Rig r;
        r.fake->SimulateSubscription(S, C, DEV, true);
        r.fake->SetTransmitCapacity(0);
        r.Notify(1);
        CHECK(BleServer_RemoveService(r.server, S) == 0);
        CHECK(r.fake->PendingNotifications() == 0);
    }
}

static void TestUnlimitedIsUnchanged() {
    Rig r;   // default fake: no flow control, every notify goes straight out
    for (uint8_t v = 1; v <= 20; ++v) CHECK(r.Notify(v) == 0);
    CHECK(r.fake->NotifyLog().size() == 20);
    CHECK(r.fake->PendingNotifications() == 0);
    CHECK(!Reported(0, "backed up"));
}

int main() {
    TestQueueSendsImmediatelyWhenEmpty();
    TestQueueKeepsOrder();
    TestQueueBoundDropsOldest();
    TestQueueRemoveAndDiscard();
    TestChunkBurstIsNotDropped();
    TestValueCapturedAtNotifyTime();
    TestOverflowDropsOldestAndReports();
    TestLastUnsubscribeClearsQueue();
    TestNoSubscriberIsNotQueued();
    TestShutdownAndRemoveClearQueue();
    TestUnlimitedIsUnchanged();
    std::printf("notify_queue: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
