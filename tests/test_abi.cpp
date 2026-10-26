// ABI / lifecycle tests: real src/common code + in-memory FakeBleServer. No hardware.
#include "IBleServer.h"
#include "FakeBleServer.h"
#include "BleMessageChunker.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace BleServer;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const int kInvalid = static_cast<int>(BleError::InvalidParameter);
static const int kNotInit = static_cast<int>(BleError::NotInitialized);
static const int kAlready = static_cast<int>(BleError::AlreadyInitialized);
static const int kOk = 0;
static const char* S = "180F";
static const char* C = "2A19";

static std::atomic<int> g_conn{0}, g_write{0}, g_err{0};
static void OnConn(const char*, bool) { ++g_conn; }
static void OnErr(int, const char*) { ++g_err; }
static void OnWrite(const char*, const uint8_t*, int) { ++g_write; }

static void* MakeInitialized() {
    void* s = BleServer_Create();
    CHECK(s != nullptr);
    CHECK(BleServer_Initialize(s, "Test", false) == kOk);
    return s;
}

static void TestNullHandles() {
    void* n = nullptr;
    uint8_t b[4] = {1, 2, 3, 4};
    char buf[16]; int len = 16;
    BleCharacteristicConfigC cc{}; cc.size = sizeof(cc); cc.uuid = C;
    BleDescriptorConfigC dc{}; dc.size = sizeof(dc); dc.uuid = "2902";
    BleServerConfigC sc{}; sc.size = sizeof(sc); sc.deviceName = "x"; sc.maxConnections = 1;

    CHECK(BleServer_AddService(n, S, true) == kInvalid);   // spec example
    CHECK(BleServer_Initialize(n, "x", false) == kInvalid);
    CHECK(BleServer_InitializeEx(n, &sc) == kInvalid);
    BleServer_Shutdown(n);
    CHECK(!BleServer_IsInitialized(n));
    CHECK(BleServer_GetLastError(n) != nullptr);
    CHECK(BleServer_GetCapabilities(n) == 0);
    CHECK(std::strcmp(BleServer_GetLastError(n), "Invalid server") == 0);
    CHECK(BleServer_GetStatus(n, buf, &len) == kInvalid);
    BleServer_LogStatus(n);
    CHECK(BleServer_RemoveService(n, S) == kInvalid);
    CHECK(BleServer_AddCharacteristic(n, S, C, 0x02, 0x01) == kInvalid);
    CHECK(BleServer_AddCharacteristicEx(n, S, &cc) == kInvalid);
    CHECK(BleServer_RemoveCharacteristic(n, S, C) == kInvalid);
    CHECK(BleServer_AddDescriptor(n, S, C, &dc) == kInvalid);
    BleServer_SetConnectionCallback(n, OnConn);
    BleServer_SetConnectionCallbackEx(n, nullptr, nullptr);
    BleServer_SetErrorCallback(n, OnErr);
    BleServer_SetErrorCallbackEx(n, nullptr, nullptr);
    CHECK(BleServer_SetCharacteristicReadCallback(n, S, C, nullptr, nullptr) == kInvalid);
    BleServer_SetCharacteristicWriteCallback(n, S, C, OnWrite);
    CHECK(BleServer_SetCharacteristicWriteCallbackEx(n, S, C, nullptr, nullptr) == kInvalid);
    CHECK(BleServer_StartAdvertising(n) == kInvalid);
    CHECK(BleServer_StopAdvertising(n) == kInvalid);
    CHECK(!BleServer_IsAdvertising(n));
    CHECK(BleServer_UpdateCharacteristicValue(n, S, C, b, 4) == kInvalid);
    CHECK(BleServer_NotifyCharacteristicChanged(n, S, C, "") == kInvalid);
    CHECK(BleServer_GetConnectedDeviceCount(n) == -1);
    CHECK(BleServer_GetConnectedDeviceId(n, 0, buf, &len) == kInvalid);
    CHECK(BleServer_DisconnectDevice(n, "dev") == kInvalid);
    BleServer_Destroy(n);  // null destroy is a no-op

    // Null chunker handle
    BleChunker_Destroy(nullptr);
    CHECK(BleChunker_SendMessage(nullptr, nullptr, S, C, b, 4) == kInvalid);
    CHECK(BleChunker_GetNextMessageSize(nullptr) == -1);
    {   // ProcessReceivedData / ProcessIncoming with null and stale chunker handles
        uint8_t out[16]; int outLen = 16;
        uint8_t ack[16]; int ackLen = 16;
        CHECK(!BleChunker_ProcessReceivedData(nullptr, b, 4, out, &outLen));
        CHECK(BleChunker_ProcessIncoming(nullptr, b, 4, ack, &ackLen) == kInvalid);
        void* c = BleChunker_Create();
        CHECK(!BleChunker_ProcessReceivedData(c, nullptr, 4, out, &outLen));       // null data
        CHECK(!BleChunker_ProcessReceivedData(c, b, -1, out, &outLen));            // negative length
        CHECK(BleChunker_ProcessIncoming(c, nullptr, 4, ack, &ackLen) == kInvalid);
        CHECK(BleChunker_ProcessIncoming(c, b, -1, ack, &ackLen) == kInvalid);
        CHECK(BleChunker_ProcessIncoming(c, b, 4, ack, &ackLen) == kInvalid);      // not a valid chunk
        BleChunker_Destroy(c);
        CHECK(!BleChunker_ProcessReceivedData(c, b, 4, out, &outLen));             // stale
        CHECK(BleChunker_ProcessIncoming(c, b, 4, ack, &ackLen) == kInvalid);      // stale
    }
    CHECK(!BleChunker_ReadNextMessage(nullptr, b, &len));
    CHECK(BleChunker_CleanupTimedOut(nullptr, 1000) == kInvalid);

    // Null string arguments on a valid handle
    void* s = MakeInitialized();
    CHECK(BleServer_AddService(s, nullptr, true) == kInvalid);
    CHECK(BleServer_AddCharacteristic(s, nullptr, C, 0x02, 0x01) == kInvalid);
    CHECK(BleServer_InitializeEx(s, nullptr) == kInvalid);
    BleServer_Destroy(s);
}

static void TestValidation() {
    void* s = MakeInitialized();
    CHECK(BleServer_AddService(s, "", true) == kInvalid);
    CHECK(BleServer_AddService(s, "XYZW", true) == kInvalid);
    CHECK(BleServer_AddService(s, "0123456789012345678901234567890", true) == kInvalid);  // 31 chars
    CHECK(BleServer_AddService(s, S, true) == kOk);
    CHECK(BleServer_AddCharacteristic(s, S, "", 0x02, 0x01) == kInvalid);
    CHECK(BleServer_AddCharacteristic(s, S, "ZZ", 0x02, 0x01) == kInvalid);
    CHECK(BleServer_AddCharacteristic(s, S, C, 0x02, 0x01) == kOk);
    CHECK(BleServer_AddService(s, S, true) == kOk);  // duplicate: the (fake) backend accepts it; ABI layer does not reject
    BleServer_Destroy(s);

    // Bad config for InitializeEx
    void* t = BleServer_Create();
    BleServerConfigC bad{}; bad.size = sizeof(bad); bad.deviceName = "x"; bad.maxConnections = -5;
    int r = BleServer_InitializeEx(t, &bad);
    CHECK(r == kOk);  // documented: non-positive maxConnections falls back to the default (10)
    CHECK(BleServer_IsInitialized(t));
    BleServer_Destroy(t);
}

static void TestLifecycle() {
    void* s = BleServer_Create();
    CHECK(!BleServer_IsInitialized(s));
    CHECK(BleServer_AddService(s, S, true) == kNotInit);
    CHECK(BleServer_Initialize(s, "T", false) == kOk);
    CHECK(BleServer_IsInitialized(s));
    CHECK(BleServer_Initialize(s, "T", false) == kAlready);
    BleServer_Shutdown(s);
    CHECK(!BleServer_IsInitialized(s));
    BleServer_Shutdown(s);  // second shutdown: harmless no-op
    CHECK(!BleServer_IsInitialized(s));
    CHECK(BleServer_Initialize(s, "T", false) == kOk);  // re-init after shutdown works
    BleServer_Destroy(s);   // destroy while initialized

    void* a = MakeInitialized();
    CHECK(BleServer_StartAdvertising(a) == kOk);
    CHECK(BleServer_IsAdvertising(a));
    BleServer_Destroy(a);   // destroy while advertising

    CHECK(BleServer_GetApiVersion() == 2);
    CHECK(std::strstr(BleServer_GetBuildInfo(), "platform=fake") != nullptr);
}

static void TestNoCallbackAfterShutdown() {
    g_conn = g_write = g_err = 0;
    void* s = MakeInitialized();
    FakeBleServer* f = FakeBleServer::Last();
    CHECK(f != nullptr);
    CHECK(BleServer_AddService(s, S, true) == kOk);
    CHECK(BleServer_AddCharacteristic(s, S, C, 0x08, 0x02) == kOk);
    BleServer_SetConnectionCallback(s, OnConn);
    BleServer_SetErrorCallback(s, OnErr);
    BleServer_SetCharacteristicWriteCallback(s, S, C, OnWrite);

    f->SimulateConnection("dev1", true);
    CHECK(f->SimulateWrite(S, C, "dev1", {1, 2, 3}));
    f->SimulateError(BleError::Unknown, "boom");
    CHECK(g_conn == 1);
    CHECK(g_write == 1);
    CHECK(g_err == 1);

    BleServer_Shutdown(s);
    f->SimulateConnection("dev1", true);
    f->SimulateWrite(S, C, "dev1", {1});
    f->SimulateError(BleError::Unknown, "late");
    CHECK(g_conn == 1);
    CHECK(g_write == 1);
    CHECK(g_err == 1);
    BleServer_Destroy(s);
}

// ---- read callback: SimulateRead + gating after Shutdown/Destroy
static std::atomic<int> g_reads{0};
static int OnRead(const char*, uint8_t* buffer, int bufferLength, void*) {
    ++g_reads;
    static const uint8_t v[3] = {9, 8, 7};
    if (buffer && bufferLength >= 3) std::memcpy(buffer, v, 3);
    return 3;
}
static void TestReadCallbackAndGating() {
    g_reads = 0;
    void* s = MakeInitialized();
    FakeBleServer* f = FakeBleServer::Last();
    f->SetRetainCallbacksOnShutdown(true);  // backend keeps its hook: only src/common can stop delivery
    CHECK(BleServer_AddService(s, S, true) == kOk);
    CHECK(BleServer_AddCharacteristic(s, S, C, 0x02, 0x01) == kOk);
    CHECK(BleServer_SetCharacteristicReadCallback(s, S, C, OnRead, nullptr) == kOk);

    std::vector<uint8_t> out;
    CHECK(f->SimulateRead(S, C, "dev1", out));
    CHECK((out == std::vector<uint8_t>{9, 8, 7}));
    CHECK(g_reads == 2);  // size query + fill

    BleServer_Shutdown(s);
    out.assign(1, 0xAA);
    CHECK(f->SimulateRead(S, C, "dev1", out));  // backend still calls the hook ...
    CHECK(out.empty());                         // ... but the user callback is gated
    CHECK(g_reads == 2);
    BleServer_Destroy(s);

    // Destroy path: a backend that outlives the handle's callbacks must not reach the user.
    void* t = MakeInitialized();
    FakeBleServer* g = FakeBleServer::Last();
    g->SetRetainCallbacksOnShutdown(true);
    CHECK(BleServer_AddService(t, S, true) == kOk);
    CHECK(BleServer_AddCharacteristic(t, S, C, 0x08, 0x02) == kOk);
    g_write = 0;
    BleServer_SetCharacteristicWriteCallback(t, S, C, OnWrite);
    CHECK(g->SimulateWrite(S, C, "d", {1}));
    CHECK(g_write == 1);
    BleServer_Shutdown(t);
    CHECK(g->SimulateWrite(S, C, "d", {1}));  // delivered to the hook, dropped by the gate
    CHECK(g_write == 1);
    BleServer_Destroy(t);
}

// ---- Shutdown / Destroy wait for a running callback (Unity frees GCHandles right after)
static std::atomic<bool> g_cbEntered{false}, g_cbRelease{false}, g_cbFinished{false};
static void BlockingWrite(const char*, const uint8_t*, int) {
    g_cbEntered = true;
    while (!g_cbRelease) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    g_cbFinished = true;
}
static void RunWaitTest(bool useDestroy) {
    g_cbEntered = g_cbRelease = g_cbFinished = false;
    void* s = MakeInitialized();
    FakeBleServer* f = FakeBleServer::Last();
    BleServer_AddService(s, S, true);
    BleServer_AddCharacteristic(s, S, C, 0x08, 0x02);
    BleServer_SetCharacteristicWriteCallback(s, S, C, BlockingWrite);
    std::thread w([&] { f->SimulateWrite(S, C, "d", {1}); });
    while (!g_cbEntered) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::atomic<bool> returned{false};
    std::thread closer([&] {
        if (useDestroy) BleServer_Destroy(s); else BleServer_Shutdown(s);
        returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!returned);        // still waiting for the running callback
    CHECK(!g_cbFinished);
    g_cbRelease = true;
    closer.join();
    CHECK(g_cbFinished);     // the callback had returned before Shutdown/Destroy did
    w.join();
    if (!useDestroy) BleServer_Destroy(s);
}
static void TestShutdownWaitsForCallbacks() {
    RunWaitTest(false);
    RunWaitTest(true);
}

// A callback that shuts the server down itself must not deadlock waiting for itself.
static void* g_selfServer = nullptr;
static std::atomic<int> g_selfCalls{0};
static void SelfShutdownConn(const char*, bool) {
    ++g_selfCalls;
    BleServer_Shutdown(g_selfServer);
}
static void TestShutdownFromInsideCallback() {
    void* s = MakeInitialized();
    FakeBleServer* f = FakeBleServer::Last();
    g_selfServer = s;
    g_selfCalls = 0;
    BleServer_SetConnectionCallback(s, SelfShutdownConn);
    f->SimulateConnection("d", true);
    CHECK(g_selfCalls == 1);
    CHECK(!BleServer_IsInitialized(s));
    BleServer_Destroy(s);
}

static void TestConcurrencySmoke() {
    void* s = MakeInitialized();
    FakeBleServer* f = FakeBleServer::Last();
    BleServer_AddService(s, S, true);
    BleServer_AddCharacteristic(s, S, C, 0x08, 0x02);
    BleServer_SetCharacteristicWriteCallback(s, S, C, OnWrite);
    void* chunker = BleChunker_Create();
    std::vector<uint8_t> big(1500, 0x5A);  // several chunks

    std::atomic<bool> stop{false};
    std::vector<std::thread> th;
    th.emplace_back([&] {  // callback setter incl. null
        int i = 0;
        while (!stop) { BleServer_SetConnectionCallback(s, (i++ & 1) ? OnConn : nullptr);
                        BleServer_SetErrorCallback(s, (i & 2) ? OnErr : nullptr); }
    });
    for (int k = 0; k < 2; ++k) th.emplace_back([&] {  // writers via the backend
        while (!stop) { f->SimulateWrite(S, C, "d", {1, 2, 3}); f->SimulateConnection("d", true);
                        uint8_t v[2] = {1, 2}; BleServer_UpdateCharacteristicValue(s, S, C, v, 2); }
    });
    th.emplace_back([&] {  // chunked sends + retransmission pass racing with shutdown
        while (!stop) {
            BleChunker_SendMessage(chunker, s, S, C, big.data(), static_cast<int>(big.size()));
            BleChunker_CleanupTimedOut(chunker, 0);
        }
    });
    th.emplace_back([&] {  // receiving side of the same chunker
        uint8_t ack[64]; int ackLen = 64;
        uint8_t msg[2048]; int msgLen = 2048;
        auto chunks = BleMessageChunker::CreateChunks(big);
        while (!stop) {
            for (auto& c : chunks) {
                ackLen = 64;
                BleChunker_ProcessIncoming(chunker, c.data(), static_cast<int>(c.size()), ack, &ackLen);
            }
            msgLen = 2048;
            BleChunker_ReadNextMessage(chunker, msg, &msgLen);
        }
    });
    th.emplace_back([&] {  // shutdown from another thread
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        BleServer_Shutdown(s);
        BleServer_Shutdown(s);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    stop = true;
    for (auto& t : th) t.join();
    BleChunker_Destroy(chunker);
    BleServer_Destroy(s);
    CHECK(true);  // reaching here without deadlock/crash (and ASan/UBSan clean) is the assertion
}

// Destroy while a write callback is executing on another thread.
static std::atomic<bool> g_inCb{false}, g_release{false};
static void SlowWrite(const char*, const uint8_t*, int) {
    g_inCb = true;
    while (!g_release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
static void TestDestroyDuringCallback() {
    void* s = MakeInitialized();
    FakeBleServer* f = FakeBleServer::Last();
    BleServer_AddService(s, S, true);
    BleServer_AddCharacteristic(s, S, C, 0x08, 0x02);
    BleServer_SetCharacteristicWriteCallback(s, S, C, SlowWrite);
    std::thread w([&] { f->SimulateWrite(S, C, "d", {1}); });
    while (!g_inCb) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::thread rel([&] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); g_release = true; });
    BleServer_Destroy(s);  // must not use-after-free (ASan)
    rel.join();
    w.join();
    CHECK(true);
}

int main() {
    TestNullHandles();
    TestValidation();
    TestLifecycle();
    TestNoCallbackAfterShutdown();
    TestReadCallbackAndGating();
    TestShutdownWaitsForCallbacks();
    TestShutdownFromInsideCallback();
    TestConcurrencySmoke();
    TestDestroyDuringCallback();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
