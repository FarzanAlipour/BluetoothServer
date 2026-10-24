// Chunker through the C ABI against the fake backend: edge cases, ACK handling, retransmission,
// bounded memory, handle safety. No hardware.
#include "IBleServer.h"
#include "BleServerInternal.h"
#include "FakeBleServer.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace BleServer;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const int kInvalid = static_cast<int>(BleError::InvalidParameter);
static const char* S = "180F";
static const char* C = "2A19";

using Bytes = std::vector<uint8_t>;
static std::atomic<int> g_errors{0};
static std::string g_lastErrorText;
static void OnErr(int, const char* msg) { ++g_errors; g_lastErrorText = msg; }

struct Rig {
    void* server = nullptr;
    FakeBleServer* fake = nullptr;
    void* sender = nullptr;
    void* receiver = nullptr;
    Rig() {
        server = BleServer_Create();
        BleServer_Initialize(server, "T", false);
        fake = FakeBleServer::Last();
        BleServer_AddService(server, S, true);
        BleServer_AddCharacteristic(server, S, C, 0x12, 0x01);
        sender = BleChunker_Create();
        receiver = BleChunker_Create();
    }
    ~Rig() {
        BleChunker_Destroy(sender);
        BleChunker_Destroy(receiver);
        BleServer_Destroy(server);
    }
    int Send(const Bytes& m) {
        return BleChunker_SendMessage(sender, server, S, C, m.data(), static_cast<int>(m.size()));
    }
    // Feeds one wire packet to the receiving chunker; returns its ACK packet.
    Bytes Deliver(const Bytes& packet) {
        uint8_t ack[64]; int ackLen = sizeof(ack);
        BleChunker_ProcessIncoming(receiver, packet.data(), static_cast<int>(packet.size()), ack, &ackLen);
        return Bytes(ack, ack + ackLen);
    }
    void Ack(const Bytes& ack) { BleChunker_ProcessIncoming(sender, ack.data(), static_cast<int>(ack.size()), nullptr, nullptr); }
    Bytes Received() {
        int n = BleChunker_GetNextMessageSize(receiver);
        Bytes out(static_cast<size_t>(n > 0 ? n : 0));
        int len = n;
        if (n < 0 || !BleChunker_ReadNextMessage(receiver, out.data(), &len)) return Bytes{0xEE, 0xEE};
        out.resize(static_cast<size_t>(len));
        return out;
    }
};

static Bytes Pattern(size_t n) {
    Bytes b(n);
    for (size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);
    return b;
}

// Round trip with every ACK delivered.
static void RoundTrip(size_t size, size_t expectedChunks) {
    Rig r;
    Bytes msg = Pattern(size);
    CHECK(r.Send(msg) == 0);
    auto wire = r.fake->ValueLog();
    CHECK(wire.size() == expectedChunks);
    for (auto& p : wire) r.Ack(r.Deliver(p));
    CHECK(r.Received() == msg);
    CHECK(BleChunker_GetNextMessageSize(r.receiver) == 0);
    CHECK(Internal::ChunkerOutstandingMessages(r.sender) == 0);   // all ACKed: released
    CHECK(Internal::ChunkerBufferedChunks(r.receiver) == 0);
}

static void TestEdgeSizes() {
    RoundTrip(0, 1);       // empty message
    RoundTrip(1, 1);
    RoundTrip(243, 1);     // exactly one chunk payload
    RoundTrip(244, 2);     // one byte over
    RoundTrip(1000, 5);    // large
    RoundTrip(243000, 1000);  // MAX_MESSAGE_SIZE (243 * 1000)

    Rig r;
    Bytes tooBig = Pattern(243001);
    CHECK(r.Send(tooBig) == kInvalid);
    CHECK(Internal::ChunkerOutstandingMessages(r.sender) == 0);
    CHECK(BleChunker_SendMessage(r.sender, r.server, S, C, nullptr, 0) == 0);  // empty, null data
    CHECK(BleChunker_SendMessage(r.sender, r.server, S, C, nullptr, 5) == kInvalid);
    CHECK(BleChunker_SendMessage(r.sender, r.server, S, C, tooBig.data(), -1) == kInvalid);
}

// Spec example: 1,000 bytes, the ACK for the 3rd chunk is lost.
static void TestLostAck() {
    Rig r;
    Bytes msg = Pattern(1000);
    CHECK(r.Send(msg) == 0);
    auto wire = r.fake->ValueLog();
    CHECK(wire.size() == 5);
    for (size_t i = 0; i < wire.size(); ++i) {
        Bytes ack = r.Deliver(wire[i]);
        if (i != 2) r.Ack(ack);  // ACK of the 3rd chunk is lost
    }
    CHECK(r.Received() == msg);
    CHECK(Internal::ChunkerOutstandingMessages(r.sender) == 1);  // still waiting for chunk 2

    CHECK(BleChunker_CleanupTimedOut(r.sender, 3600000) == 0);   // not timed out yet: nothing resent
    CHECK(BleChunker_CleanupTimedOut(r.sender, 0) == 1);         // timeout: oldest unacked chunk resent
    auto after = r.fake->ValueLog();
    CHECK(after.size() == 6);
    CHECK(after.back() == wire[2]);                              // exactly the lost chunk

    Bytes ack = r.Deliver(after.back());                         // receiver sees a duplicate: ACKs, no re-delivery
    CHECK(BleChunker_GetNextMessageSize(r.receiver) == 0);
    r.Ack(ack);
    CHECK(Internal::ChunkerOutstandingMessages(r.sender) == 0);
    CHECK(Internal::ChunkerBufferedChunks(r.receiver) == 0);
    CHECK(BleChunker_CleanupTimedOut(r.sender, 0) == 0);
}

// The chunk itself is lost: retransmission completes the message.
static void TestLostChunk() {
    Rig r;
    Bytes msg = Pattern(1000);
    CHECK(r.Send(msg) == 0);
    auto wire = r.fake->ValueLog();
    for (size_t i = 0; i < wire.size(); ++i) {
        if (i == 2) continue;
        r.Ack(r.Deliver(wire[i]));
    }
    CHECK(BleChunker_GetNextMessageSize(r.receiver) == 0);       // incomplete
    CHECK(BleChunker_CleanupTimedOut(r.sender, 0) == 1);
    r.Ack(r.Deliver(r.fake->ValueLog().back()));
    CHECK(r.Received() == msg);
    CHECK(Internal::ChunkerOutstandingMessages(r.sender) == 0);
}

static void TestDuplicateAndOutOfOrderAcks() {
    Rig r;
    Bytes msg = Pattern(1000);
    CHECK(r.Send(msg) == 0);
    auto wire = r.fake->ValueLog();
    std::vector<Bytes> acks;
    for (auto& p : wire) acks.push_back(r.Deliver(p));

    r.Ack(acks[3]); r.Ack(acks[3]);  // duplicate
    r.Ack(acks[3]);
    r.Ack(acks[1]);                  // out of order
    r.Ack(acks[0]);
    CHECK(Internal::ChunkerOutstandingMessages(r.sender) == 1);
    r.Ack(acks[4]);
    CHECK(Internal::ChunkerOutstandingMessages(r.sender) == 1);  // chunk 2 still missing
    r.Ack(acks[2]);
    CHECK(Internal::ChunkerOutstandingMessages(r.sender) == 0);
    r.Ack(acks[2]);                  // ACK for a finished message: harmless
    CHECK(Internal::ChunkerOutstandingMessages(r.sender) == 0);
    CHECK(r.Received() == msg);

    // Duplicate and out-of-order DATA chunks reassemble correctly, exactly once.
    Rig r2;
    CHECK(r2.Send(msg) == 0);
    auto w2 = r2.fake->ValueLog();
    for (int i : {4, 2, 2, 0, 3, 3, 1, 1, 0}) r2.Deliver(w2[static_cast<size_t>(i)]);
    CHECK(r2.Received() == msg);
    CHECK(BleChunker_GetNextMessageSize(r2.receiver) == 0);
}

static void TestRetryBoundAndFailure() {
    g_errors = 0; g_lastErrorText.clear();
    Rig r;
    BleServer_SetErrorCallback(r.server, OnErr);
    CHECK(r.Send(Pattern(500)) == 0);
    for (int i = 0; i < Internal::kMaxChunkRetries; ++i) {
        CHECK(BleChunker_CleanupTimedOut(r.sender, 0) == 1);
    }
    CHECK(g_errors == 0);
    CHECK(BleChunker_CleanupTimedOut(r.sender, 0) == 0);          // 6th timeout: message failed
    CHECK(g_errors == 1);
    CHECK(g_lastErrorText == "chunk 0 not acknowledged");
    CHECK(Internal::ChunkerOutstandingMessages(r.sender) == 0);   // memory released
    CHECK(BleChunker_CleanupTimedOut(r.sender, -1) == kInvalid);
    CHECK(BleChunker_CleanupTimedOut(r.sender, 0) == 0);
}

static void TestBoundedMemory() {
    Rig r;
    // Sender: unacknowledged messages never accumulate beyond the cap.
    for (int i = 0; i < 50; ++i) CHECK(r.Send(Pattern(1000)) == 0);
    CHECK(Internal::ChunkerOutstandingMessages(r.sender) == Internal::kMaxOutstandingMessages);

    // Repeated retransmission cycles do not grow anything.
    for (int i = 0; i < 3; ++i) BleChunker_CleanupTimedOut(r.sender, 0);
    CHECK(Internal::ChunkerOutstandingMessages(r.sender) <= Internal::kMaxOutstandingMessages);

    // Receiver: first chunks of many different messages (never completed) stay bounded.
    auto wire = r.fake->ValueLog();
    for (int i = 0; i < 40; ++i) r.Deliver(wire[static_cast<size_t>(i) * 5 % 250]);
    size_t buffered = Internal::ChunkerBufferedChunks(r.receiver);
    CHECK(buffered <= Internal::kMaxPartialMessages * 5);

    // Hostile header: totalChunks far above the limit must not allocate or be accepted.
    Bytes evil = wire[0];
    evil[9] = evil[10] = evil[11] = evil[12] = 0xFF;
    uint8_t ack[64]; int ackLen = sizeof(ack);
    CHECK(BleChunker_ProcessIncoming(r.receiver, evil.data(), static_cast<int>(evil.size()), ack, &ackLen) == kInvalid);
    CHECK(Internal::ChunkerBufferedChunks(r.receiver) == buffered);
    Bytes garbage = {0x01, 0x02};
    CHECK(BleChunker_ProcessIncoming(r.receiver, garbage.data(), 2, ack, &ackLen) == kInvalid);
}

static void TestHandleSafety() {
    size_t servers0 = Internal::LiveServerHandles(), chunkers0 = Internal::LiveChunkerHandles();
    {
        void* s = BleServer_Create();
        void* c = BleChunker_Create();
        CHECK(Internal::LiveServerHandles() == servers0 + 1);
        CHECK(Internal::LiveChunkerHandles() == chunkers0 + 1);
        BleServer_Destroy(s);
        BleServer_Destroy(s);            // double destroy: no-op, no crash
        CHECK(BleServer_AddService(s, S, true) == kInvalid);   // stale handle rejected, not dereferenced
        CHECK(!BleServer_IsInitialized(s));
        BleServer_Shutdown(s);
        uint8_t b[1] = {1};
        CHECK(BleChunker_SendMessage(c, s, S, C, b, 1) == kInvalid);   // stale server handle
        BleChunker_Destroy(c);
        BleChunker_Destroy(c);
        CHECK(BleChunker_CleanupTimedOut(c, 0) == kInvalid);           // stale chunker handle
        int len = 4;
        CHECK(!BleChunker_ReadNextMessage(c, b, &len));
        int bogus; 
        CHECK(BleServer_AddService(&bogus, S, true) == kInvalid);      // never-valid pointer
        CHECK(!BleServer_IsAdvertising(&bogus));
    }
    CHECK(Internal::LiveServerHandles() == servers0);              // no leaked handles
    CHECK(Internal::LiveChunkerHandles() == chunkers0);
}

// A message-failure error callback runs unlocked: it may call back into the API.
static void* g_reentrantServer = nullptr;
static std::atomic<int> g_reentrant{0};
static void ReentrantErr(int, const char*) {
    BleServer_GetLastError(g_reentrantServer);
    BleServer_SetErrorCallback(g_reentrantServer, nullptr);  // would self-deadlock if a lock were held
    ++g_reentrant;
}
static void TestCallbackRunsUnlocked() {
    Rig r;
    g_reentrantServer = r.server;
    g_reentrant = 0;
    BleServer_SetErrorCallback(r.server, ReentrantErr);
    r.fake->SimulateError(BleError::Unknown, "x");
    CHECK(g_reentrant == 1);
    r.fake->SimulateError(BleError::Unknown, "y");   // callback cleared itself
    CHECK(g_reentrant == 1);
}

int main() {
    TestEdgeSizes();
    TestLostAck();
    TestLostChunk();
    TestDuplicateAndOutOfOrderAcks();
    TestRetryBoundAndFailure();
    TestBoundedMemory();
    TestHandleSafety();
    TestCallbackRunsUnlocked();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
