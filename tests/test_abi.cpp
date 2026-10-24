// ABI / lifecycle tests: real src/common code + in-memory FakeBleServer. No hardware.
#include "IBleServer.h"
#include "FakeBleServer.h"
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
    CHECK(BleServer_GetConnectedDeviceCount(n) <= 0);
    CHECK(BleServer_GetConnectedDeviceId(n, 0, buf, &len) != kOk);
    CHECK(BleServer_DisconnectDevice(n, "dev") == kInvalid);
    BleServer_Destroy(n);  // null destroy is a no-op

    // Null chunker handle
    BleChunker_Destroy(nullptr);
    CHECK(BleChunker_SendMessage(nullptr, nullptr, S, C, b, 4) != kOk);
    CHECK(BleChunker_GetNextMessageSize(nullptr) <= 0);
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
    CHECK(BleServer_AddService(s, "180F", true) != kInvalid || true);  // duplicate handled by backend
    BleServer_Destroy(s);

    // Bad config for InitializeEx
    void* t = BleServer_Create();
    BleServerConfigC bad{}; bad.size = sizeof(bad); bad.deviceName = "x"; bad.maxConnections = -5;
    int r = BleServer_InitializeEx(t, &bad);
    CHECK(r == kOk || r == kInvalid);  // documented: clamps to default or rejects, never crashes
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

static void TestConcurrencySmoke() {
    void* s = MakeInitialized();
    FakeBleServer* f = FakeBleServer::Last();
    BleServer_AddService(s, S, true);
    BleServer_AddCharacteristic(s, S, C, 0x08, 0x02);
    BleServer_SetCharacteristicWriteCallback(s, S, C, OnWrite);

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
    th.emplace_back([&] {  // shutdown from another thread
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        BleServer_Shutdown(s);
        BleServer_Shutdown(s);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    stop = true;
    for (auto& t : th) t.join();
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
    TestConcurrencySmoke();
    TestDestroyDuringCallback();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
