// Long (prepared) write tests: the shared LongWriteAssembler rules, then the same rules end to end
// through the C ABI against the in-memory FakeBleServer. No hardware.
#include "IBleServer.h"
#include "FakeBleServer.h"
#include "LongWriteAssembler.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace BleServer;
using LongWrite::Status;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::vector<uint8_t> Pattern(size_t n, uint8_t seed = 0) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(seed + i);
    return v;
}

static std::vector<uint8_t> Slice(const std::vector<uint8_t>& v, size_t from, size_t len) {
    return std::vector<uint8_t>(v.begin() + from, v.begin() + from + len);
}

// Splits `msg` into fragments of `chunk` bytes, as an ATT client does for MTU = chunk + 5.
static std::vector<LongWrite::Fragment> Fragments(const std::string& key, const std::vector<uint8_t>& msg, size_t chunk) {
    std::vector<LongWrite::Fragment> out;
    for (size_t off = 0; off < msg.size(); off += chunk) {
        out.push_back({key, off, Slice(msg, off, std::min(chunk, msg.size() - off))});
    }
    return out;
}

static bool KnownAB(const std::string& key, std::vector<uint8_t>& out) {
    out.clear();
    return key == "A" || key == "B";
}

// ---------------------------------------------------------------- assembler unit tests

static void TestApplyFragment() {
    std::vector<uint8_t> v = {9, 9, 9};
    CHECK(LongWrite::ApplyFragment(v, 0, {1, 2}) == Status::Ok);      // offset 0 replaces the value
    CHECK((v == std::vector<uint8_t>{1, 2}));
    CHECK(LongWrite::ApplyFragment(v, 2, {3, 4}) == Status::Ok);      // append at the end
    CHECK((v == std::vector<uint8_t>{1, 2, 3, 4}));
    CHECK(LongWrite::ApplyFragment(v, 1, {7}) == Status::Ok);         // overlap: keep prefix, replace rest
    CHECK((v == std::vector<uint8_t>{1, 7}));
    CHECK(LongWrite::ApplyFragment(v, 3, {5}) == Status::InvalidOffset);  // gap
    CHECK((v == std::vector<uint8_t>{1, 7}));                         // unchanged on error
    CHECK(LongWrite::ApplyFragment(v, 2, {}) == Status::Ok);          // empty fragment at end is fine
    CHECK((v == std::vector<uint8_t>{1, 7}));

    std::vector<uint8_t> big = Pattern(500);
    CHECK(LongWrite::ApplyFragment(big, 500, Pattern(12)) == Status::Ok);   // exactly 512
    CHECK(big.size() == 512);
    CHECK(LongWrite::ApplyFragment(big, 512, {1}) == Status::InvalidAttributeValueLength);
    CHECK(LongWrite::ApplyFragment(big, 0, Pattern(513)) == Status::InvalidAttributeValueLength);
    CHECK(big.size() == 512);
    std::vector<uint8_t> empty;
    CHECK(LongWrite::ApplyFragment(empty, 600, {1}) == Status::InvalidOffset);  // offset checked first
}

static void TestBatchSingleWrite() {
    auto r = LongWrite::AssembleBatch({{"A", 0, {1, 2, 3}}}, KnownAB);
    CHECK(r.status == Status::Ok);
    CHECK(r.values.size() == 1);
    CHECK(r.values[0].key == "A");
    CHECK((r.values[0].value == std::vector<uint8_t>{1, 2, 3}));
}

static void TestBatchLongWrite() {
    // nRF Connect, MTU 23: 300 bytes -> 18-byte Prepare Write fragments.
    const auto msg = Pattern(300, 5);
    auto r = LongWrite::AssembleBatch(Fragments("A", msg, 18), KnownAB);
    CHECK(r.status == Status::Ok);
    CHECK(r.values.size() == 1);
    CHECK(r.values[0].value == msg);

    // 512 is the maximum and is accepted, 513 is not.
    CHECK(LongWrite::AssembleBatch(Fragments("A", Pattern(512), 18), KnownAB).status == Status::Ok);
    auto tooLong = LongWrite::AssembleBatch(Fragments("A", Pattern(513), 18), KnownAB);
    CHECK(tooLong.status == Status::InvalidAttributeValueLength);
    CHECK(tooLong.failedIndex == 28);   // 28 * 18 = 504, last fragment would end at 513
    CHECK(tooLong.values.empty());
}

static void TestBatchGapAndOrder() {
    auto frags = Fragments("A", Pattern(60), 18);   // offsets 0, 18, 36, 54
    auto gap = frags;
    gap.erase(gap.begin() + 1);                     // 0, 36, 54
    auto r = LongWrite::AssembleBatch(gap, KnownAB);
    CHECK(r.status == Status::InvalidOffset);
    CHECK(r.failedIndex == 1);
    CHECK(r.values.empty());

    auto noStart = LongWrite::AssembleBatch({{"A", 4, {1}}}, KnownAB);   // nothing at offset 0
    CHECK(noStart.status == Status::InvalidOffset);

    // Out of order is processed in delivery order (ATT queue order): the later fragment at offset 18
    // first is a gap.
    std::swap(frags[0], frags[1]);
    CHECK(LongWrite::AssembleBatch(frags, KnownAB).status == Status::InvalidOffset);
}

static void TestBatchMultipleCharacteristics() {
    const auto a = Pattern(40, 1);
    const auto b = Pattern(25, 100);
    std::vector<LongWrite::Fragment> mixed = {
        {"B", 0, Slice(b, 0, 18)},
        {"A", 0, Slice(a, 0, 18)},
        {"A", 18, Slice(a, 18, 18)},
        {"B", 18, Slice(b, 18, 7)},
        {"A", 36, Slice(a, 36, 4)},
    };
    auto r = LongWrite::AssembleBatch(mixed, KnownAB);
    CHECK(r.status == Status::Ok);
    CHECK(r.values.size() == 2);
    CHECK(r.values[0].key == "B");                  // first-appearance order
    CHECK(r.values[0].value == b);
    CHECK(r.values[1].key == "A");
    CHECK(r.values[1].value == a);

    // Unknown characteristic anywhere -> the whole batch fails.
    mixed.push_back({"X", 0, {1}});
    auto bad = LongWrite::AssembleBatch(mixed, KnownAB);
    CHECK(bad.status == Status::AttributeNotFound);
    CHECK(bad.failedIndex == 5);
    CHECK(bad.values.empty());
}

// ---------------------------------------------------------------- C ABI + FakeBleServer

static const char* S = "180F";
static const char* C1 = "2A19";
static const char* C2 = "2A1A";

struct Received { std::string chr; std::vector<uint8_t> data; };
static std::vector<Received> g_received;
static void OnWrite1(const char*, const uint8_t* d, int n) { g_received.push_back({C1, std::vector<uint8_t>(d, d + n)}); }
static void OnWrite2(const char*, const uint8_t* d, int n) { g_received.push_back({C2, std::vector<uint8_t>(d, d + n)}); }

static void* MakeServer(FakeBleServer*& fake) {
    void* s = BleServer_Create();
    CHECK(s != nullptr);
    CHECK(BleServer_Initialize(s, "LongWrite", false) == 0);
    fake = FakeBleServer::Last();
    CHECK(fake != nullptr);
    CHECK(BleServer_AddService(s, S, true) == 0);
    CHECK(BleServer_AddCharacteristic(s, S, C1, 0x08, 0x02) == 0);
    CHECK(BleServer_AddCharacteristic(s, S, C2, 0x08, 0x02) == 0);
    BleServer_SetCharacteristicWriteCallback(s, S, C1, OnWrite1);
    BleServer_SetCharacteristicWriteCallback(s, S, C2, OnWrite2);
    g_received.clear();
    return s;
}

static std::vector<FakeBleServer::WriteFragment> FakeFragments(const char* chr, const std::vector<uint8_t>& msg, size_t chunk) {
    std::vector<FakeBleServer::WriteFragment> out;
    for (size_t off = 0; off < msg.size(); off += chunk) {
        out.push_back({S, chr, off, Slice(msg, off, std::min(chunk, msg.size() - off))});
    }
    return out;
}

static void TestAbiBatchDeliversOneMessage() {
    FakeBleServer* f = nullptr;
    void* s = MakeServer(f);
    const auto msg = Pattern(300, 7);
    CHECK(f->SimulateWriteBatch("central", FakeFragments(C1, msg, 18)) == Status::Ok);
    CHECK(g_received.size() == 1);                  // one callback, not one per fragment
    CHECK(g_received[0].chr == C1);
    CHECK(g_received[0].data == msg);
    CHECK(f->LastValue(S, C1) == msg);

    // Two characteristics in one Execute Write: one callback each, full values.
    g_received.clear();
    const auto a = Pattern(50, 1), b = Pattern(30, 2);
    auto frags = FakeFragments(C1, a, 18);
    auto more = FakeFragments(C2, b, 18);
    frags.insert(frags.end(), more.begin(), more.end());
    CHECK(f->SimulateWriteBatch("central", frags) == Status::Ok);
    CHECK(g_received.size() == 2);
    CHECK(g_received.size() == 2 && g_received[0].chr == C1 && g_received[0].data == a);
    CHECK(g_received.size() == 2 && g_received[1].chr == C2 && g_received[1].data == b);
    BleServer_Destroy(s);
}

static void TestAbiBatchErrorsApplyNothing() {
    FakeBleServer* f = nullptr;
    void* s = MakeServer(f);
    const std::vector<uint8_t> before = {0xAA};
    CHECK(f->SimulateWriteBatch("central", {{S, C1, 0, before}}) == Status::Ok);
    g_received.clear();

    // Valid C2 part + gap in C1: nothing stored, no callback for either.
    auto frags = FakeFragments(C2, Pattern(20), 18);
    auto c1 = FakeFragments(C1, Pattern(60), 18);
    c1.erase(c1.begin() + 2);
    frags.insert(frags.end(), c1.begin(), c1.end());
    CHECK(f->SimulateWriteBatch("central", frags) == Status::InvalidOffset);
    CHECK(g_received.empty());
    CHECK(f->LastValue(S, C1) == before);
    CHECK(f->LastValue(S, C2).empty());

    CHECK(f->SimulateWriteBatch("central", FakeFragments(C1, Pattern(513), 18)) == Status::InvalidAttributeValueLength);
    CHECK(f->SimulateWriteBatch("central", {{S, C1, 0, {1}}, {S, "2A99", 0, {1}}}) == Status::AttributeNotFound);
    CHECK(g_received.empty());
    CHECK(f->LastValue(S, C1) == before);
    BleServer_Destroy(s);
}

static void TestAbiFragmentAtOffset() {
    // Linux / Windows path: fragments arrive one call at a time.
    FakeBleServer* f = nullptr;
    void* s = MakeServer(f);
    const auto msg = Pattern(100, 3);
    for (size_t off = 0; off < msg.size(); off += 40) {
        CHECK(f->SimulateWriteAtOffset(S, C1, "central", off, Slice(msg, off, std::min<size_t>(40, msg.size() - off))) == Status::Ok);
    }
    CHECK(f->LastValue(S, C1) == msg);
    CHECK(g_received.size() == 3);
    CHECK(!g_received.empty() && g_received.back().data == msg);   // last callback carries the full value

    g_received.clear();
    CHECK(f->SimulateWriteAtOffset(S, C1, "central", 101, {1}) == Status::InvalidOffset);
    CHECK(f->SimulateWriteAtOffset(S, C1, "central", 100, Pattern(413)) == Status::InvalidAttributeValueLength);
    CHECK(f->SimulateWriteAtOffset(S, "2A99", "central", 0, {1}) == Status::AttributeNotFound);
    CHECK(g_received.empty());
    CHECK(f->LastValue(S, C1) == msg);

    CHECK(f->SimulateWriteAtOffset(S, C1, "central", 0, {5}) == Status::Ok);   // a new plain write replaces
    CHECK((f->LastValue(S, C1) == std::vector<uint8_t>{5}));
    BleServer_Destroy(s);
}

int main() {
    TestApplyFragment();
    TestBatchSingleWrite();
    TestBatchLongWrite();
    TestBatchGapAndOrder();
    TestBatchMultipleCharacteristics();
    TestAbiBatchDeliversOneMessage();
    TestAbiBatchErrorsApplyNothing();
    TestAbiFragmentAtOffset();
    std::printf("long_write: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
