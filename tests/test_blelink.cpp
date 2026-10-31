// test_blelink.cpp - BleLink v1 codec and engine tests (docs/PROTOCOL.md).
//
//  1. Codec against docs/protocol-test-vectors.json.
//  2. Scripted peer: one engine, raw packets in, captured packets out (exact protocol reactions).
//  3. Simulated link: engines wired back to back with back-pressure and fault injection, on a fake clock.
//  4. Threaded smoke test: worker threads, real clock, concurrent link thread.
#include "BleLinkCodec.h"
#include "BleLinkEngine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace BleServer::BleLink;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                              \
        }                                                                              \
    } while (0)

#ifndef BLELINK_VECTORS_PATH
#define BLELINK_VECTORS_PATH "docs/protocol-test-vectors.json"
#endif

using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------------------

static Bytes Hex(const std::string& h) {
    Bytes out;
    CHECK(h.size() % 2 == 0);
    for (size_t i = 0; i < h.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoul(h.substr(i, 2), nullptr, 16)));
    return out;
}

static uint32_t HexU32(const std::string& h) { return static_cast<uint32_t>(std::stoul(h, nullptr, 16)); }

static Bytes RulePayload(size_t n) {  // vectors' payloadRule: byte[i] = (i * 7 + 3) mod 256
    Bytes b(n);
    for (size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
    return b;
}

static Bytes Pattern(size_t n, uint32_t seed) {
    Bytes b(n);
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        b[i] = static_cast<uint8_t>(x >> 24);
    }
    return b;
}

// Minimal JSON reader (objects, arrays, strings without escapes beyond \" \\ \/, numbers, bools, null).
struct Json {
    enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json& operator[](const std::string& key) const {
        for (const auto& kv : obj) {
            if (kv.first == key) return kv.second;
        }
        static const Json null;
        return null;
    }
    bool Has(const std::string& key) const {
        for (const auto& kv : obj) {
            if (kv.first == key) return true;
        }
        return false;
    }
    uint64_t U() const { return static_cast<uint64_t>(num); }
};

struct JsonParser {
    const std::string& s;
    size_t i = 0;
    explicit JsonParser(const std::string& text) : s(text) {}
    void Ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
    }
    Json Value() {
        Ws();
        CHECK(i < s.size());
        Json j;
        const char c = s[i];
        if (c == '{') {
            j.t = Json::T::Obj;
            ++i;
            Ws();
            if (s[i] == '}') { ++i; return j; }
            for (;;) {
                Ws();
                std::string k = Str();
                Ws();
                CHECK(s[i] == ':');
                ++i;
                j.obj.emplace_back(k, Value());
                Ws();
                if (s[i] == ',') { ++i; continue; }
                CHECK(s[i] == '}');
                ++i;
                return j;
            }
        }
        if (c == '[') {
            j.t = Json::T::Arr;
            ++i;
            Ws();
            if (s[i] == ']') { ++i; return j; }
            for (;;) {
                j.arr.push_back(Value());
                Ws();
                if (s[i] == ',') { ++i; continue; }
                CHECK(s[i] == ']');
                ++i;
                return j;
            }
        }
        if (c == '"') {
            j.t = Json::T::Str;
            j.str = Str();
            return j;
        }
        if (s.compare(i, 4, "true") == 0) { j.t = Json::T::Bool; j.b = true; i += 4; return j; }
        if (s.compare(i, 5, "false") == 0) { j.t = Json::T::Bool; i += 5; return j; }
        if (s.compare(i, 4, "null") == 0) { i += 4; return j; }
        j.t = Json::T::Num;
        size_t used = 0;
        j.num = std::stod(s.substr(i, 32), &used);
        CHECK(used > 0);
        i += used;
        return j;
    }
    std::string Str() {
        CHECK(s[i] == '"');
        ++i;
        std::string out;
        while (s[i] != '"') {
            if (s[i] == '\\') {
                ++i;
                CHECK(s[i] == '"' || s[i] == '\\' || s[i] == '/');
            }
            out.push_back(s[i++]);
        }
        ++i;
        return out;
    }
};

static Json LoadVectors() {
    std::ifstream f(BLELINK_VECTORS_PATH, std::ios::binary);
    CHECK(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    JsonParser p(text);
    return p.Value();
}

// ---------------------------------------------------------------------------------------------------------
// 1. Codec
// ---------------------------------------------------------------------------------------------------------

static void TestCodecVectors(const Json& v) {
    for (const auto& c : v["crc32"]["cases"].arr) {
        Bytes in = c.Has("inputHex") ? Hex(c["inputHex"].str) : Bytes(c["inputAscii"].str.begin(), c["inputAscii"].str.end());
        CHECK(Crc32(0, in.data(), in.size()) == HexU32(c["crc32"].str));
        // chaining
        if (in.size() > 2) CHECK(Crc32(Crc32(0, in.data(), 2), in.data() + 2, in.size() - 2) == HexU32(c["crc32"].str));
    }

    for (const auto& pk : v["packets"].arr) {
        const Bytes raw = Hex(pk["hex"].str);
        CHECK(raw.size() == pk["length"].U());
        const Json& f = pk["fields"];
        Packet p;
        CHECK(Decode(raw.data(), raw.size(), p) == DecodeResult::Ok);
        CHECK(p.type == f["type"].U());
        CHECK(p.seq == f["seq"].U());
        Bytes enc;
        const Bytes payload = f.Has("payloadHex") ? Hex(f["payloadHex"].str) : Bytes();
        switch (static_cast<PacketType>(p.type)) {
        case PacketType::Hello: {
            CHECK(p.hello.version == f["version"].U());
            CHECK(p.hello.maxPacket == f["maxPacket"].U());
            CHECK(p.hello.maxMessage == f["maxMessage"].U());
            CHECK(p.hello.window == f["window"].U());
            CHECK(p.hello.flags == f["flags"].U());
            CHECK(p.hello.timeout == f["timeout"].U());
            EncodeHello(enc, p.seq, p.hello);
            break;
        }
        case PacketType::MsgStart:
            CHECK(p.msgId == f["msgId"].U());
            CHECK(p.totalLen == f["totalLen"].U());
            CHECK(p.crc == HexU32(f["crc32"].str));
            CHECK(Bytes(p.payload, p.payload + p.payloadLen) == payload);
            CHECK(Crc32(0, payload.data(), payload.size()) == p.crc || p.totalLen != payload.size());
            EncodeMsgStart(enc, p.seq, p.msgId, p.totalLen, p.crc, payload.data(), payload.size());
            break;
        case PacketType::MsgData:
            CHECK(p.msgId == f["msgId"].U());
            CHECK(Bytes(p.payload, p.payload + p.payloadLen) == payload);
            EncodeMsgData(enc, p.seq, p.msgId, payload.data(), payload.size());
            break;
        case PacketType::Ack:
            CHECK(p.msgId == f["msgId"].U());
            CHECK(p.status == f["status"].U());
            EncodeAck(enc, p.seq, p.msgId, p.status);
            break;
        case PacketType::Resume:
            CHECK(p.msgId == f["msgId"].U());
            CHECK(p.offset == f["offset"].U());
            EncodeResume(enc, p.seq, p.msgId, p.offset);
            break;
        case PacketType::Cancel:
            CHECK(p.msgId == f["msgId"].U());
            CHECK(p.reason == f["reason"].U());
            EncodeCancel(enc, p.seq, p.msgId, p.reason);
            break;
        case PacketType::Progress:
            CHECK(p.msgId == f["msgId"].U());
            CHECK(p.bytesReceived == f["bytesReceived"].U());
            EncodeProgress(enc, p.seq, p.msgId, p.bytesReceived);
            break;
        case PacketType::MsgResend:
            CHECK(p.msgId == f["msgId"].U());
            CHECK(p.offset == f["offset"].U());
            CHECK(Bytes(p.payload, p.payload + p.payloadLen) == payload);
            EncodeMsgResend(enc, p.seq, p.msgId, p.offset, payload.data(), payload.size());
            break;
        }
        CHECK(enc == raw);
    }

    {
        const Bytes info = Hex(v["info"]["hex"].str);
        Hello h;
        CHECK(DecodeInfo(info.data(), info.size(), h));
        Bytes enc;
        EncodeInfo(enc, h);
        CHECK(enc == info);
        CHECK(!DecodeInfo(info.data(), info.size() - 1, h));
    }

    for (const auto& c : v["messageSplits"]["cases"].arr) {
        const Bytes msg = RulePayload(c["messageLength"].U());
        CHECK(Crc32(0, msg.data(), msg.size()) == HexU32(c["crc32"].str));
        const uint16_t ps = static_cast<uint16_t>(c["packetSize"].U());
        CHECK(StartCapacity(ps) == c["startPayloadBytes"].U());
        CHECK(DataCapacity(ps) == c["dataPayloadBytes"].U());
        auto pkts = SplitMessage(ps, static_cast<uint16_t>(c["msgId"].U()), static_cast<uint8_t>(c["firstSeq"].U()),
                                 msg.data(), msg.size());
        CHECK(pkts.size() == c["packetCount"].U());
        CHECK(pkts.size() == c["packetsHex"].arr.size());
        for (size_t i = 0; i < pkts.size(); ++i) {
            CHECK(pkts[i] == Hex(c["packetsHex"].arr[i].str));
            CHECK(pkts[i].size() <= ps);
        }
    }

    for (const auto& c : v["boundaryCases20"]["cases"].arr) {
        const Bytes msg = RulePayload(c["messageLength"].U());
        CHECK(Crc32(0, msg.data(), msg.size()) == HexU32(c["crc32"].str));
        auto pkts = SplitMessage(20, 5, 1, msg.data(), msg.size());
        CHECK(pkts.size() == c["packetsHex"].arr.size());
        for (size_t i = 0; i < pkts.size(); ++i) CHECK(pkts[i] == Hex(c["packetsHex"].arr[i].str));
    }
    std::printf("  codec vectors OK\n");
}

static void TestCodecEdges() {
    Packet p;
    const uint8_t one[] = {0x02};
    CHECK(Decode(nullptr, 0, p) == DecodeResult::Empty);
    CHECK(Decode(one, 1, p) == DecodeResult::Empty);
    const uint8_t unknown[] = {0x7F, 0x05, 1, 2, 3};
    CHECK(Decode(unknown, sizeof unknown, p) == DecodeResult::UnknownType);
    CHECK(p.type == 0x7F && p.seq == 5);
    const uint8_t shortAck[] = {0x04, 0x01, 0x00, 0x00};
    CHECK(Decode(shortAck, sizeof shortAck, p) == DecodeResult::TooShort);
    const uint8_t shortStart[] = {0x02, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(Decode(shortStart, sizeof shortStart, p) == DecodeResult::TooShort);
    // Trailing bytes after fixed packets are ignored (forward compatibility).
    const uint8_t longAck[] = {0x04, 0x01, 0x07, 0x00, 0x00, 0xEE, 0xFF};
    CHECK(Decode(longAck, sizeof longAck, p) == DecodeResult::Ok);
    CHECK(p.msgId == 7 && p.status == 0);
    Bytes hello = Hex("010001f40000000001040128");
    hello.push_back(0x99);
    CHECK(Decode(hello.data(), hello.size(), p) == DecodeResult::Ok);
    CHECK(p.hello.maxPacket == 244 && p.hello.timeout == 40);
    // Empty MSG_DATA (n = 0) is legal.
    const uint8_t emptyData[] = {0x03, 0x02, 0x01, 0x00};
    CHECK(Decode(emptyData, sizeof emptyData, p) == DecodeResult::Ok);
    CHECK(p.payloadLen == 0);
    CHECK(ClampPacketSize(0) == 20 && ClampPacketSize(19) == 20 && ClampPacketSize(600) == 512 &&
          ClampPacketSize(244) == 244);
    CHECK(TimeoutUnitsToMs(0) == 10000 && TimeoutUnitsToMs(4) == 1000 && TimeoutUnitsToMs(255) == 63750);
    // Empty message: one MSG_START, crc 0.
    auto pkts = SplitMessage(20, 2, 2, nullptr, 0);
    CHECK(pkts.size() == 1 && pkts[0] == Hex("020202000000000000000000"));
    std::printf("  codec edges OK\n");
}

// ---------------------------------------------------------------------------------------------------------
// 2. Scripted peer
// ---------------------------------------------------------------------------------------------------------

struct CaptureTransport : ITransport {
    std::vector<Bytes> sent;
    std::deque<TransportResult> script;  // results for the next calls; Sent when empty
    int disconnects = 0;
    TransportResult SendPacket(const PeerId&, const uint8_t* data, size_t len) override {
        TransportResult r = TransportResult::Sent;
        if (!script.empty()) {
            r = script.front();
            script.pop_front();
        }
        if (r == TransportResult::Sent) sent.emplace_back(data, data + len);
        return r;
    }
    void RequestDisconnect(const PeerId&) override { ++disconnects; }
};

struct Scripted {
    uint64_t now = 1000;
    CaptureTransport t;
    Engine e;
    uint8_t peerSeq = 0;
    std::vector<Event> events;
    size_t cursor = 0;  // first packet in t.sent not yet consumed by Next()

    explicit Scripted(const Config& c) : e(t, c, [this] { return now; }) {}

    void Feed(const Bytes& b) { e.OnPacket("P", b.data(), b.size()); }
    // Encodes with the scripted peer's own seq counter.
    void Hello(const BleServer::BleLink::Hello& h) {
        Bytes b;
        EncodeHello(b, 0, h);
        peerSeq = 1;
        Feed(b);
    }
    void Start(uint16_t id, const Bytes& msg, size_t n) {
        Bytes b;
        EncodeMsgStart(b, peerSeq++, id, static_cast<uint32_t>(msg.size()), Crc32(0, msg.data(), msg.size()),
                       msg.data(), n);
        Feed(b);
    }
    void StartRaw(uint16_t id, uint32_t total, uint32_t crc, const Bytes& payload) {
        Bytes b;
        EncodeMsgStart(b, peerSeq++, id, total, crc, payload.data(), payload.size());
        Feed(b);
    }
    void Data(uint16_t id, const uint8_t* p, size_t n) {
        Bytes b;
        EncodeMsgData(b, peerSeq++, id, p, n);
        Feed(b);
    }
    void Ack(uint16_t id, uint8_t status) {
        Bytes b;
        EncodeAck(b, peerSeq++, id, status);
        Feed(b);
    }
    void Resume(uint16_t id, uint32_t off) {
        Bytes b;
        EncodeResume(b, peerSeq++, id, off);
        Feed(b);
    }
    void Pump() {
        e.Pump();
        Event ev;
        while (e.PollEvent(ev)) events.push_back(std::move(ev));
    }
    bool Next(Packet& p, Bytes& storage) {
        if (cursor >= t.sent.size()) return false;
        storage = t.sent[cursor++];
        CHECK(Decode(storage.data(), storage.size(), p) == DecodeResult::Ok);
        return true;
    }
    size_t Count(EventType type) const {
        size_t n = 0;
        for (const auto& ev : events) n += ev.type == type ? 1 : 0;
        return n;
    }
    const Event* Last(EventType type) const {
        for (auto it = events.rbegin(); it != events.rend(); ++it) {
            if (it->type == type) return &*it;
        }
        return nullptr;
    }
};

static BleServer::BleLink::Hello ClientHello(uint16_t maxPacket = 244, uint8_t window = 4, uint32_t maxMessage = 16777216,
                                             uint8_t flags = kFlagProgress) {
    BleServer::BleLink::Hello h;
    h.maxPacket = maxPacket;
    h.maxMessage = maxMessage;
    h.window = window;
    h.flags = flags;
    h.timeout = 40;
    return h;
}

// Server engine with a scripted client that already did the handshake.
static void OpenServer(Scripted& sc, const BleServer::BleLink::Hello& h, uint16_t osLimit = 0) {
    sc.e.OnPeerConnected("P");
    sc.e.OnPeerSubscribed("P");
    if (osLimit) sc.e.SetPeerOsLimit("P", osLimit);
    sc.Hello(h);
    sc.Pump();
    Packet p;
    Bytes raw;
    CHECK(sc.Next(p, raw));
    CHECK(p.type == static_cast<uint8_t>(PacketType::Hello) && p.seq == 0);
    CHECK(sc.Count(EventType::PeerReady) == 1);
}

static void TestScriptedHandshake() {
    Config c;
    c.role = Role::Server;
    c.maxPacket = 509;
    c.window = 3;
    c.timeoutUnits = 80;
    Scripted sc(c);
    sc.e.OnPeerConnected("P");
    // Not subscribed yet: data before HELLO is ignored, HELLO is accepted but the answer waits for the CCCD.
    sc.Start(0, Bytes{1, 2, 3}, 3);
    sc.Hello(ClientHello(244));
    sc.Pump();
    CHECK(sc.t.sent.empty());
    sc.e.OnPeerSubscribed("P");
    sc.e.SetPeerOsLimit("P", 300);
    sc.Pump();
    CHECK(sc.t.sent.size() == 1);
    Packet p;
    CHECK(Decode(sc.t.sent[0].data(), sc.t.sent[0].size(), p) == DecodeResult::Ok);
    CHECK(p.type == 1 && p.seq == 0 && p.hello.version == 1 && p.hello.maxPacket == 509 && p.hello.window == 3 &&
          p.hello.maxMessage == 16u * 1024 * 1024 && p.hello.flags == kFlagProgress && p.hello.timeout == 80);
    const Event* ready = sc.Last(EventType::PeerReady);
    CHECK(ready != nullptr);
    CHECK(ready->info.txPacketSize == 244);  // min(peer maxPacket 244, os 300)
    CHECK(ready->info.timeoutMs == 20000);   // max(80, 40) units
    PeerInfo info;
    CHECK(sc.e.GetPeerInfo("P", info) && info.ready);
    CHECK(sc.e.InfoValue() == Bytes(sc.t.sent[0].begin() + 2, sc.t.sent[0].end()));
    // No message was delivered (the pre-HELLO MSG_START was ignored).
    CHECK(sc.Count(EventType::MessageReceived) == 0);
    std::printf("  scripted handshake OK\n");
}

static void TestScriptedInterleavingVector(const Json& v) {
    Config c;
    Scripted sc(c);
    OpenServer(sc, ClientHello());
    for (const auto& h : v["interleaving"]["packetsHex"].arr) sc.Feed(Hex(h.str));
    sc.Pump();
    // Completion order: 11 then 10; ACK bytes exactly as in the vectors (server seq 1, 2).
    std::vector<uint16_t> order;
    for (const auto& ev : sc.events) {
        if (ev.type == EventType::MessageReceived) order.push_back(ev.msgId);
    }
    CHECK(order.size() == 2 && order[0] == 11 && order[1] == 10);
    const auto& acks = v["interleaving"]["ackPacketsHex"].arr;
    CHECK(sc.t.sent.size() == 1 + acks.size());
    for (size_t i = 0; i < acks.size(); ++i) CHECK(sc.t.sent[1 + i] == Hex(acks[i].str));
    for (const auto& ev : sc.events) {
        if (ev.type != EventType::MessageReceived) continue;
        if (ev.msgId == 10) CHECK(ev.data == RulePayload(60));
        if (ev.msgId == 11) CHECK(ev.data == Bytes({'p', 'i', 'n', 'g'}));
    }
    std::printf("  scripted interleaving vector OK\n");
}

static void TestScriptedResumeVector(const Json& v) {
    const Json& r = v["resume"];
    Config c;
    Scripted sc(c);
    OpenServer(sc, ClientHello());
    const auto& orig = r["originalPacketsHex"].arr;
    for (size_t i = 0; i < orig.size(); ++i) {
        if (i == r["lostIndex"].U()) continue;
        sc.Feed(Hex(orig[i].str));
    }
    sc.Pump();
    CHECK(sc.t.sent.size() == 2);
    CHECK(sc.t.sent[1] == Hex(r["resumeHex"].str));
    CHECK(sc.Count(EventType::MessageReceived) == 0);
    for (const auto& h : r["senderResendPacketsHex"].arr) sc.Feed(Hex(h.str));
    sc.Pump();
    const Event* got = sc.Last(EventType::MessageReceived);
    CHECK(got != nullptr && got->msgId == 20 && got->data == RulePayload(60));
    Packet p;
    CHECK(Decode(sc.t.sent.back().data(), sc.t.sent.back().size(), p) == DecodeResult::Ok);
    CHECK(p.type == static_cast<uint8_t>(PacketType::Ack) && p.msgId == 20 && p.status == 0);

    // Sender side: the engine sends 60 bytes at packetSize 20, gets RESUME(0, 24) and must continue with
    // MSG_RESEND(24) + MSG_DATA carrying exactly the vector's resend payloads.
    Scripted tx(c);
    OpenServer(tx, ClientHello(20));
    const SendOutcome so = tx.e.Send("P", RulePayload(60));
    CHECK(so.code == SendOutcome::Code::Ok && so.handle != 0);
    tx.Pump();
    Packet q;
    Bytes raw;
    size_t n = 0;
    while (tx.Next(q, raw)) {
        CHECK(raw.size() <= 20);
        CHECK(q.msgId == 0);
        ++n;
    }
    CHECK(n == 5);
    tx.Resume(0, 24);
    tx.Pump();
    std::vector<Bytes> resent;
    while (tx.Next(q, raw)) resent.push_back(raw);
    const auto& expect = r["senderResendPacketsHex"].arr;
    CHECK(resent.size() == expect.size());
    for (size_t i = 0; i < resent.size(); ++i) {
        Packet a, b;
        const Bytes e = Hex(expect[i].str);
        CHECK(Decode(resent[i].data(), resent[i].size(), a) == DecodeResult::Ok);
        CHECK(Decode(e.data(), e.size(), b) == DecodeResult::Ok);
        CHECK(a.type == b.type && a.offset == b.offset);
        CHECK(Bytes(a.payload, a.payload + a.payloadLen) == Bytes(b.payload, b.payload + b.payloadLen));
    }
    // A second RESUME for the same gap is ignored only until the next packet is sent; here the stream
    // already went out, so an older offset restarts again (bounded by the 8-restart limit).
    tx.Ack(0, 0);
    tx.Pump();
    CHECK(tx.Count(EventType::MessageSent) == 1);
    std::printf("  scripted resume vector OK\n");
}

static void TestScriptedReceiverLimits() {
    Config c;
    c.maxMessage = 1000;
    c.window = 2;
    Scripted sc(c);
    OpenServer(sc, ClientHello());
    Packet p;
    Bytes raw;
    // too large -> early ACK 2, later data ignored silently (no RESUME)
    const Bytes big = Pattern(2000, 1);
    sc.Start(1, big, 232);
    sc.Data(1, big.data() + 232, 240);
    sc.Pump();
    CHECK(sc.Next(p, raw) && p.type == 4 && p.msgId == 1 && p.status == 2);
    CHECK(!sc.Next(p, raw));
    // window: two incomplete messages fill it; the third START gets ACK 3
    const Bytes m = Pattern(900, 2);
    sc.Start(2, m, 100);
    sc.Start(3, m, 100);
    sc.Start(4, m, 100);
    sc.Pump();
    CHECK(sc.Next(p, raw) && p.type == 4 && p.msgId == 4 && p.status == 3);
    CHECK(!sc.Next(p, raw));
    // finish 2 and 3, the second with a wrong CRC -> ACK 1
    sc.Data(2, m.data() + 100, 800);
    Bytes bad(m.begin() + 100, m.end());
    bad[5] ^= 0x40;
    sc.Data(3, bad.data(), bad.size());
    sc.Pump();
    CHECK(sc.Next(p, raw) && p.type == 4 && p.msgId == 2 && p.status == 0);
    CHECK(sc.Next(p, raw) && p.type == 4 && p.msgId == 3 && p.status == 1);
    CHECK(sc.Count(EventType::MessageReceived) == 1);
    // duplicate START for completed id 2 -> stored ACK 0 again, no second delivery
    sc.Start(2, m, 100);
    sc.Data(2, m.data() + 100, 800);  // ignored (remembered)
    sc.Pump();
    CHECK(sc.Next(p, raw) && p.type == 4 && p.msgId == 2 && p.status == 0);
    CHECK(!sc.Next(p, raw));
    CHECK(sc.Count(EventType::MessageReceived) == 1);
    // CRC-failed id 3 is not remembered: a resend is received normally
    sc.Start(3, m, 200);
    sc.Data(3, m.data() + 200, 700);
    sc.Pump();
    CHECK(sc.Next(p, raw) && p.type == 4 && p.msgId == 3 && p.status == 0);
    CHECK(sc.Count(EventType::MessageReceived) == 2);
    // more bytes than totalLen -> ACK 1
    const Bytes s10 = Pattern(10, 3);
    sc.Start(5, s10, 4);
    sc.Data(5, m.data(), 20);
    sc.Pump();
    CHECK(sc.Next(p, raw) && p.type == 4 && p.msgId == 5 && p.status == 1);
    // MSG_DATA for an unknown id -> RESUME(id, 0) exactly once
    sc.Data(77, m.data(), 10);
    sc.Data(77, m.data(), 10);
    sc.Pump();
    CHECK(sc.Next(p, raw) && p.type == 5 && p.msgId == 77 && p.offset == 0);
    CHECK(!sc.Next(p, raw));
    // CANCEL frees a partial message without ACK
    sc.Start(6, m, 100);
    {
        Bytes b;
        EncodeCancel(b, sc.peerSeq++, 6, 0);
        sc.Feed(b);
    }
    sc.Pump();
    CHECK(!sc.Next(p, raw));
    PeerInfo info;
    CHECK(sc.e.GetPeerInfo("P", info) && info.receiving == 0);
    // unknown packet type: ignored but its seq counts (no gap afterwards)
    {
        Bytes b = {0x55, sc.peerSeq++, 1, 2, 3};
        sc.Feed(b);
    }
    // 0-byte and 1-byte messages
    sc.Start(8, Bytes(), 0);
    sc.Start(9, Bytes{0xAB}, 1);
    sc.Pump();
    CHECK(sc.Next(p, raw) && p.type == 4 && p.msgId == 8 && p.status == 0);
    CHECK(sc.Next(p, raw) && p.type == 4 && p.msgId == 9 && p.status == 0);
    CHECK(sc.e.GetStats().seqGaps == 0);
    const Event* last = sc.Last(EventType::MessageReceived);
    CHECK(last && last->msgId == 9 && last->data == Bytes{0xAB});
    std::printf("  scripted receiver limits OK\n");
}

static void TestScriptedReceiverGapAndIdle() {
    Config c;
    Scripted sc(c);
    OpenServer(sc, ClientHello());
    Packet p;
    Bytes raw;
    const Bytes m = Pattern(1000, 4);
    sc.Start(1, m, 232);
    sc.Data(1, m.data() + 232, 240);
    ++sc.peerSeq;  // lost packet
    sc.Data(1, m.data() + 712, 240);  // stale: dropped (resync)
    sc.Pump();
    CHECK(sc.Next(p, raw) && p.type == 5 && p.msgId == 1 && p.offset == 472);
    // MSG_RESEND with the wrong offset -> RESUME(received) once
    {
        Bytes b;
        EncodeMsgResend(b, sc.peerSeq++, 1, 600, m.data() + 600, 100);
        sc.Feed(b);
        EncodeMsgResend(b, sc.peerSeq++, 1, 700, m.data() + 700, 100);
        sc.Feed(b);
    }
    sc.Pump();
    CHECK(sc.Next(p, raw) && p.type == 5 && p.msgId == 1 && p.offset == 472);
    CHECK(!sc.Next(p, raw));
    // idle timeout: RESUME again after T, then give up after 3 RESUMEs without progress
    for (int i = 0; i < 3; ++i) {
        sc.now += 10000;
        sc.Pump();
        CHECK(sc.Next(p, raw) && p.type == 5 && p.offset == 472);
    }
    sc.now += 10000;
    sc.Pump();
    CHECK(!sc.Next(p, raw));
    PeerInfo info;
    CHECK(sc.e.GetPeerInfo("P", info) && info.receiving == 0);
    // a correct repair after a fresh start works
    sc.Start(2, m, 232);
    sc.Data(2, m.data() + 232, 240);
    ++sc.peerSeq;
    sc.Pump();
    CHECK(!sc.Next(p, raw));  // gap is detected on the next packet, not before
    {
        Bytes b;
        EncodeMsgResend(b, sc.peerSeq++, 2, 472, m.data() + 472, 236);
        sc.Feed(b);
    }
    sc.Data(2, m.data() + 708, 240);
    sc.Data(2, m.data() + 948, 52);
    sc.Pump();
    CHECK(sc.Next(p, raw) && p.type == 5 && p.msgId == 2 && p.offset == 472);  // RESUME from the gap
    CHECK(sc.Next(p, raw) && p.type == 4 && p.msgId == 2 && p.status == 0);
    CHECK(sc.Last(EventType::MessageReceived)->data == m);
    std::printf("  scripted receiver gap/idle OK\n");
}

static void TestScriptedSender() {
    Config c;
    Packet p;
    Bytes raw;
    {
        // ACK statuses map to failure reasons; CRC is retried 3 times.
        Scripted sc(c);
        OpenServer(sc, ClientHello(244, 8));
        uint32_t h[5];
        for (int i = 0; i < 5; ++i) h[i] = sc.e.Send("P", Pattern(50, i)).handle;
        sc.Pump();
        while (sc.Next(p, raw)) {}
        sc.Ack(1, 2);
        sc.Ack(2, 3);
        sc.Ack(3, 4);
        sc.Ack(4, 9);
        sc.Pump();
        std::map<uint32_t, FailReason> fails;
        for (const auto& ev : sc.events) {
            if (ev.type == EventType::MessageFailed) fails[ev.handle] = ev.reason;
        }
        CHECK(fails[h[1]] == FailReason::TooLarge && fails[h[2]] == FailReason::Busy &&
              fails[h[3]] == FailReason::Rejected && fails[h[4]] == FailReason::Rejected);
        for (int i = 0; i < 3; ++i) {
            sc.Ack(0, 1);
            sc.Pump();
            CHECK(sc.Next(p, raw) && p.type == 2 && p.msgId == 0);
            while (sc.Next(p, raw)) CHECK(p.type == 3 && p.msgId == 0);
        }
        sc.Ack(0, 1);
        sc.Pump();
        CHECK(sc.Last(EventType::MessageFailed)->handle == h[0]);
        CHECK(sc.Last(EventType::MessageFailed)->reason == FailReason::Crc);
        CHECK(sc.e.GetStats().crcRetries == 3);
    }
    {
        // ACK timeout: 3 resends with the same msgId, then fail with timeout and CANCEL(reason 1).
        Scripted sc(c);
        OpenServer(sc, ClientHello());
        const uint32_t h = sc.e.Send("P", Pattern(500, 9)).handle;
        sc.Pump();
        while (sc.Next(p, raw)) {}
        for (int i = 0; i < 3; ++i) {
            sc.now += 9999;
            sc.Pump();
            CHECK(!sc.Next(p, raw));
            sc.now += 1;
            sc.Pump();
            CHECK(sc.Next(p, raw) && p.type == 2 && p.msgId == 0);
            while (sc.Next(p, raw)) CHECK(p.type == 3);
        }
        sc.now += 10000;
        sc.Pump();
        CHECK(sc.Next(p, raw) && p.type == 6 && p.msgId == 0 && p.reason == 1);
        const Event* f = sc.Last(EventType::MessageFailed);
        CHECK(f && f->handle == h && f->reason == FailReason::Timeout);
    }
    {
        // RESUME limits: 8 restarts, then link_unreliable + CANCEL. Unknown RESUMEs are ignored.
        Scripted sc(c);
        OpenServer(sc, ClientHello());
        const uint32_t h = sc.e.Send("P", Pattern(3000, 5)).handle;
        sc.Pump();
        while (sc.Next(p, raw)) {}
        sc.Resume(42, 0);
        for (int i = 0; i < 8; ++i) {
            sc.Resume(0, 232);
            sc.Pump();
            CHECK(sc.Next(p, raw) && p.type == 8 && p.offset == 232);
            while (sc.Next(p, raw)) {}
        }
        sc.Resume(0, 232);
        sc.Pump();
        CHECK(sc.Next(p, raw) && p.type == 6 && p.reason == 1);
        CHECK(sc.Last(EventType::MessageFailed)->handle == h);
        CHECK(sc.Last(EventType::MessageFailed)->reason == FailReason::LinkUnreliable);
    }
    {
        // Duplicate RESUME suppression while the restart has not sent anything yet (transport busy).
        Scripted sc(c);
        OpenServer(sc, ClientHello());
        sc.e.Send("P", Pattern(3000, 6));
        sc.Pump();
        while (sc.Next(p, raw)) {}
        sc.t.script = {TransportResult::Busy};
        sc.Resume(0, 472);
        sc.Resume(0, 472);
        sc.Resume(0, 232);
        sc.Pump();  // busy: nothing out
        CHECK(!sc.Next(p, raw));
        sc.e.OnTransportReady("P");
        sc.Pump();
        CHECK(sc.Next(p, raw) && p.type == 8 && p.offset == 472);
        CHECK(sc.e.GetStats().restarts == 1);
    }
    {
        // Peer window 1: one message in flight; the next starts only after the ACK. Ids increase.
        Scripted sc(c);
        OpenServer(sc, ClientHello(244, 1));
        sc.e.Send("P", Pattern(8, 1));  // 8 bytes = exactly one MSG_START at packetSize 20
        sc.e.Send("P", Pattern(8, 2));
        sc.e.Send("P", Pattern(8, 3));
        sc.Pump();
        CHECK(sc.Next(p, raw) && p.type == 2 && p.msgId == 0);
        CHECK(!sc.Next(p, raw));
        PeerInfo info;
        CHECK(sc.e.GetPeerInfo("P", info) && info.inFlight == 1 && info.queued == 2);
        sc.Ack(0, 0);
        sc.Pump();
        CHECK(sc.Next(p, raw) && p.type == 2 && p.msgId == 1);
        CHECK(!sc.Next(p, raw));
    }
    {
        // Oversize, not ready, cancel.
        Scripted sc(c);
        CHECK(sc.e.Send("P", Bytes(10)).code == SendOutcome::Code::NotReady);
        OpenServer(sc, ClientHello(244, 4, 100));
        CHECK(sc.e.Send("P", Bytes(101)).code == SendOutcome::Code::TooLarge);
        CHECK(sc.e.Send("P", Bytes(100)).code == SendOutcome::Code::Ok);
        const uint32_t h = sc.e.Send("P", Bytes(100, 7)).handle;
        sc.Pump();
        while (sc.Next(p, raw)) {}
        sc.e.Cancel(h);
        sc.Pump();
        CHECK(sc.Next(p, raw) && p.type == 6 && p.msgId == 1 && p.reason == 0);
        CHECK(sc.Last(EventType::MessageFailed)->reason == FailReason::Cancelled);
    }
    {
        // Transport errors: retried after backoff, dropped after 3 (the peer then sees a seq gap).
        Scripted sc(c);
        OpenServer(sc, ClientHello());
        sc.t.script = {TransportResult::Error, TransportResult::Error, TransportResult::Error};
        sc.e.Send("P", Pattern(10, 1));
        sc.e.Send("P", Pattern(10, 2));
        sc.Pump();
        CHECK(!sc.Next(p, raw));
        sc.now += 50;
        sc.Pump();
        CHECK(!sc.Next(p, raw));
        sc.now += 50;
        sc.Pump();  // third error: packet given up, seq consumed
        sc.now += 50;
        sc.Pump();
        CHECK(sc.Next(p, raw) && p.type == 2 && p.msgId == 1 && p.seq == 2);
        CHECK(sc.e.GetStats().packetsDropped == 1);
    }
    std::printf("  scripted sender OK\n");
}

static void TestScriptedResetAndClient() {
    Packet p;
    Bytes raw;
    {
        // HELLO while READY resets: in-flight fails with reset, partial receive is dropped, server answers.
        Config c;
        Scripted sc(c);
        OpenServer(sc, ClientHello());
        const uint32_t h = sc.e.Send("P", Pattern(1000, 1)).handle;
        sc.Start(5, Pattern(500, 2), 100);
        sc.Pump();
        while (sc.Next(p, raw)) {}
        sc.Hello(ClientHello(182));
        sc.Pump();
        CHECK(sc.Next(p, raw) && p.type == 1 && p.seq == 0);
        const Event* f = sc.Last(EventType::MessageFailed);
        CHECK(f && f->handle == h && f->reason == FailReason::Reset);
        CHECK(sc.Count(EventType::PeerReady) == 2);
        CHECK(sc.Last(EventType::PeerReady)->info.txPacketSize == 20);  // default OS limit
        PeerInfo info;
        CHECK(sc.e.GetPeerInfo("P", info) && info.receiving == 0 && info.inFlight == 0);
        // version 0 HELLO is ignored
        BleServer::BleLink::Hello v0 = ClientHello();
        v0.version = 0;
        sc.Hello(v0);
        sc.Pump();
        CHECK(!sc.Next(p, raw));
        // unsubscribe -> PeerGone
        sc.e.OnPeerUnsubscribed("P");
        sc.Pump();
        CHECK(sc.Count(EventType::PeerGone) == 1);
    }
    {
        // Client: HELLO on subscribe, resent on timeout (3 times), then disconnect + PeerGone(timeout).
        Config c;
        c.role = Role::Client;
        Scripted sc(c);
        sc.e.OnPeerConnected("P");
        sc.e.OnPeerSubscribed("P");
        sc.Pump();
        CHECK(sc.Next(p, raw) && p.type == 1);
        for (int i = 0; i < 3; ++i) {
            sc.now += 10000;
            sc.Pump();
            CHECK(sc.Next(p, raw) && p.type == 1 && p.seq == 0);
        }
        sc.now += 10000;
        sc.Pump();
        CHECK(!sc.Next(p, raw));
        CHECK(sc.t.disconnects == 1);
        CHECK(sc.Count(EventType::PeerGone) == 1 && sc.Last(EventType::PeerGone)->reason == FailReason::Timeout);
    }
    {
        // Client: a duplicate server HELLO is dropped (no answer, no reset); a new one resets and is answered.
        Config c;
        c.role = Role::Client;
        Scripted sc(c);
        sc.e.OnPeerConnected("P");
        sc.e.OnPeerSubscribed("P");
        sc.Pump();
        CHECK(sc.Next(p, raw) && p.type == 1);
        sc.Hello(ClientHello(509));
        sc.Pump();
        CHECK(sc.Count(EventType::PeerReady) == 1);
        sc.Hello(ClientHello(509));
        sc.Pump();
        CHECK(!sc.Next(p, raw));
        sc.Ack(99, 0);  // something in between
        sc.Hello(ClientHello(509));
        sc.Pump();
        CHECK(sc.Next(p, raw) && p.type == 1);
        CHECK(sc.Count(EventType::PeerReady) == 2);
    }
    std::printf("  scripted reset/client OK\n");
}

// ---------------------------------------------------------------------------------------------------------
// 3. Simulated link
// ---------------------------------------------------------------------------------------------------------

enum class Action { Deliver, Drop, Duplicate };

class Sim {
public:
    struct Node;
    struct Channel {
        Node* from = nullptr;
        Node* to = nullptr;
        PeerId toId;    // how `from` names `to`
        PeerId fromId;  // how `to` names `from`
        std::deque<Bytes> q;
        size_t cap = 8;
        uint32_t busyPct = 0;
        bool wantReady = false;
        bool up = true;
        uint64_t index = 0;  // packets accepted so far
        size_t maxSeen = 0;
        std::function<Action(uint64_t, Bytes&)> filter;
    };
    struct Transport : ITransport {
        Sim* sim = nullptr;
        Node* node = nullptr;
        TransportResult SendPacket(const PeerId& peer, const uint8_t* data, size_t len) override {
            Channel* ch = sim->Route(node, peer);
            if (ch == nullptr || !ch->up) return TransportResult::Error;
            if (ch->q.size() >= ch->cap || (ch->busyPct && sim->Rand() % 100 < ch->busyPct)) {
                ch->wantReady = true;
                return TransportResult::Busy;
            }
            Bytes b(data, data + len);
            ch->maxSeen = std::max(ch->maxSeen, len);
            const Action a = ch->filter ? ch->filter(ch->index, b) : Action::Deliver;
            ++ch->index;
            if (a == Action::Drop) return TransportResult::Sent;  // a stack that lost it silently
            ch->q.push_back(b);
            if (a == Action::Duplicate) ch->q.push_back(b);
            return TransportResult::Sent;
        }
    };
    struct Node {
        std::string name;
        Transport transport;
        std::unique_ptr<Engine> engine;
        std::vector<Event> events;
    };

    uint64_t now = 1000;
    uint32_t rng = 12345;

    uint32_t Rand() {
        rng = rng * 1103515245u + 12345u;
        return (rng >> 8) & 0xFFFFFF;
    }

    Node* AddNode(const std::string& name, Config cfg) {
        auto n = std::make_unique<Node>();
        n->name = name;
        n->transport.sim = this;
        n->transport.node = n.get();
        n->engine = std::make_unique<Engine>(n->transport, cfg, [this] { return now; });
        nodes_.push_back(std::move(n));
        return nodes_.back().get();
    }

    // client <-> server: two channels; server names the client by its node name, the client names the server "S".
    std::pair<Channel*, Channel*> Connect(Node* client, Node* server, uint16_t clientOs, uint16_t serverOs,
                                          size_t cap = 8) {
        auto up = std::make_unique<Channel>();
        auto down = std::make_unique<Channel>();
        up->from = client; up->to = server; up->toId = "S"; up->fromId = client->name; up->cap = cap;
        down->from = server; down->to = client; down->toId = client->name; down->fromId = "S"; down->cap = cap;
        Channel* u = up.get();
        Channel* d = down.get();
        routes_[{client, "S"}] = u;
        routes_[{server, client->name}] = d;
        channels_.push_back(std::move(up));
        channels_.push_back(std::move(down));
        server->engine->OnPeerConnected(client->name);
        client->engine->OnPeerConnected("S");
        client->engine->SetPeerOsLimit("S", clientOs);
        server->engine->SetPeerOsLimit(client->name, serverOs);
        server->engine->OnPeerSubscribed(client->name);  // CCCD write
        client->engine->OnPeerSubscribed("S");           // client: subscribed -> HELLO
        return {u, d};
    }

    void Disconnect(Node* client, Node* server) {
        Channel* u = Route(client, "S");
        Channel* d = Route(server, client->name);
        u->up = d->up = false;
        u->q.clear();
        d->q.clear();
        routes_.erase({client, "S"});
        routes_.erase({server, client->name});
        server->engine->OnPeerDisconnected(client->name);
        client->engine->OnPeerDisconnected("S");
    }

    Channel* Route(Node* n, const PeerId& peer) {
        auto it = routes_.find({n, peer});
        return it == routes_.end() ? nullptr : it->second;
    }

    // Runs until pred() or until nothing can happen any more / maxMs of simulated time passed.
    bool Run(const std::function<bool()>& pred, uint64_t maxMs = 600000) {
        const uint64_t limit = now + maxMs;
        for (uint64_t iter = 0; iter < 50000000; ++iter) {
            uint64_t wait = kNoDeadline;
            for (auto& n : nodes_) {
                wait = std::min(wait, n->engine->Pump());
                Event ev;
                while (n->engine->PollEvent(ev)) n->events.push_back(std::move(ev));
            }
            if (pred()) return true;
            bool delivered = false;
            for (auto& ch : channels_) {
                while (ch->up && !ch->q.empty()) {
                    Bytes b = std::move(ch->q.front());
                    ch->q.pop_front();
                    ch->to->engine->OnPacket(ch->fromId, b.data(), b.size());
                    delivered = true;
                }
                if (ch->up && ch->wantReady) {
                    ch->wantReady = false;
                    ch->from->engine->OnTransportReady(ch->toId);
                    delivered = true;
                }
            }
            if (delivered || wait == 0) continue;
            if (wait == kNoDeadline) return pred();
            now += wait;
            if (now > limit) return false;
        }
        return false;
    }

    static size_t Count(const Node* n, EventType t) {
        size_t c = 0;
        for (const auto& ev : n->events) c += ev.type == t ? 1 : 0;
        return c;
    }
    static std::vector<const Event*> All(const Node* n, EventType t) {
        std::vector<const Event*> v;
        for (const auto& ev : n->events) {
            if (ev.type == t) v.push_back(&ev);
        }
        return v;
    }

private:
    std::vector<std::unique_ptr<Node>> nodes_;
    std::vector<std::unique_ptr<Channel>> channels_;
    std::map<std::pair<Node*, PeerId>, Channel*> routes_;
};

static Config ServerCfg(uint16_t maxPacket = 512) {
    Config c;
    c.role = Role::Server;
    c.maxPacket = maxPacket;
    return c;
}

static Config ClientCfg(uint16_t maxPacket = 512) {
    Config c;
    c.role = Role::Client;
    c.maxPacket = maxPacket;
    return c;
}

static bool Ready(Sim::Node* n) { return Sim::Count(n, EventType::PeerReady) > 0; }

// Sends `msgs` from a to b (b names a as `aId`, a names b as `bId`) and checks delivery and ACKs.
static void Exchange(Sim& sim, Sim::Node* a, const PeerId& bId, Sim::Node* b, const std::vector<Bytes>& msgs,
                     bool requireOrder = false, uint64_t maxMs = 600000) {
    const size_t recvBefore = Sim::Count(b, EventType::MessageReceived);
    const size_t sentBefore = Sim::Count(a, EventType::MessageSent);
    std::set<uint32_t> handles;
    for (const auto& m : msgs) {
        const SendOutcome o = a->engine->Send(bId, m);
        CHECK(o.code == SendOutcome::Code::Ok);
        handles.insert(o.handle);
    }
    const bool ok = sim.Run(
        [&] {
            return Sim::Count(b, EventType::MessageReceived) - recvBefore == msgs.size() &&
                   Sim::Count(a, EventType::MessageSent) - sentBefore == msgs.size();
        },
        maxMs);
    CHECK(ok);
    auto got = Sim::All(b, EventType::MessageReceived);
    std::vector<const Event*> mine(got.begin() + static_cast<std::ptrdiff_t>(recvBefore), got.end());
    // every message delivered exactly once with the right content
    std::multiset<Bytes> want(msgs.begin(), msgs.end());
    for (const Event* ev : mine) {
        auto it = want.find(ev->data);
        CHECK(it != want.end());
        want.erase(it);
    }
    CHECK(want.empty());
    if (requireOrder) {
        for (size_t i = 0; i < msgs.size(); ++i) CHECK(mine[i]->data == msgs[i]);
    }
    auto sent = Sim::All(a, EventType::MessageSent);
    for (size_t i = sentBefore; i < sent.size(); ++i) CHECK(handles.count(sent[i]->handle) == 1);
}

static void TestSimPacketSizes() {
    for (uint16_t ps : {uint16_t(20), uint16_t(182), uint16_t(244), uint16_t(509)}) {
        Sim sim;
        auto* s = sim.AddNode("S", ServerCfg(ps));
        auto* c = sim.AddNode("C", ClientCfg(ps));
        auto chans = sim.Connect(c, s, ps, ps);
        CHECK(sim.Run([&] { return Ready(s) && Ready(c); }));
        CHECK(Sim::All(s, EventType::PeerReady)[0]->info.txPacketSize == ps);
        CHECK(Sim::All(c, EventType::PeerReady)[0]->info.txPacketSize == ps);
        const size_t sc = StartCapacity(ps), dc = DataCapacity(ps);
        std::vector<Bytes> msgs = {Bytes(),           Bytes{0x42},          Pattern(sc, 1),         Pattern(sc + 1, 2),
                                   Pattern(sc + dc, 3), Pattern(sc + dc + 1, 4), Pattern(1000, 5),     Pattern(100000, 6)};
        Exchange(sim, c, "S", s, msgs);
        Exchange(sim, s, "C", c, msgs);
        // both directions at the same time
        for (const auto& m : msgs) {
            CHECK(c->engine->Send("S", m).code == SendOutcome::Code::Ok);
        }
        Exchange(sim, s, "C", c, msgs);
        CHECK(sim.Run([&] {
            return Sim::Count(s, EventType::MessageReceived) == 2 * msgs.size() &&
                   Sim::Count(c, EventType::MessageSent) == 2 * msgs.size();
        }));
        CHECK(Sim::Count(s, EventType::MessageFailed) == 0 && Sim::Count(c, EventType::MessageFailed) == 0);
        CHECK(chans.first->maxSeen <= ps && chans.second->maxSeen <= ps);
        CHECK(chans.first->maxSeen == ps && chans.second->maxSeen == ps);
        const Stats st = s->engine->GetStats();
        CHECK(st.seqGaps == 0 && st.resumesSent == 0 && st.ackTimeouts == 0);
    }
    // Asymmetric: the client writes 182-byte packets, the server notifies 244-byte packets.
    Sim sim;
    auto* s = sim.AddNode("S", ServerCfg(512));
    auto* c = sim.AddNode("C", ClientCfg(244));
    auto chans = sim.Connect(c, s, 182, 509);
    CHECK(sim.Run([&] { return Ready(s) && Ready(c); }));
    Exchange(sim, c, "S", s, {Pattern(5000, 1)});
    Exchange(sim, s, "C", c, {Pattern(5000, 2)});
    CHECK(chans.first->maxSeen == 182 && chans.second->maxSeen == 244);
    std::printf("  sim packet sizes 20/182/244/509 OK\n");
}

static void TestSimHuge() {
    Sim sim;
    auto* s = sim.AddNode("S", ServerCfg(512));
    auto* c = sim.AddNode("C", ClientCfg(512));
    auto chans = sim.Connect(c, s, 244, 509, 16);
    chans.first->busyPct = 5;
    chans.second->busyPct = 5;
    CHECK(sim.Run([&] { return Ready(s) && Ready(c); }));
    const Bytes big = Pattern(16u * 1024 * 1024, 77);
    Exchange(sim, c, "S", s, {big}, false, 3600000);
    Exchange(sim, s, "C", c, {big}, false, 3600000);
    // Progress was reported on both sides (sending, receiving and the peer's PROGRESS packets).
    bool sending = false, peerReceived = false, receiving = false;
    for (const auto* ev : Sim::All(c, EventType::Progress)) {
        sending |= ev->progressKind == ProgressKind::Sending;
        peerReceived |= ev->progressKind == ProgressKind::PeerReceived;
        receiving |= ev->progressKind == ProgressKind::Receiving;
    }
    CHECK(sending && peerReceived && receiving);
    // 17 MiB more than the limit is refused locally.
    CHECK(c->engine->Send("S", Bytes(16u * 1024 * 1024 + 1)).code == SendOutcome::Code::TooLarge);
    std::printf("  sim 16 MiB both directions OK\n");
}

static void TestSimBackPressure() {
    Sim sim;
    auto* s = sim.AddNode("S", ServerCfg());
    auto* c = sim.AddNode("C", ClientCfg());
    auto chans = sim.Connect(c, s, 244, 244, 1);
    chans.first->busyPct = 40;
    chans.second->busyPct = 40;
    CHECK(sim.Run([&] { return Ready(s) && Ready(c); }));
    std::vector<Bytes> msgs;
    for (int i = 0; i < 12; ++i) msgs.push_back(Pattern(static_cast<size_t>(i) * 3001 + 1, i));
    Exchange(sim, c, "S", s, msgs);
    Exchange(sim, s, "C", c, msgs);
    CHECK(c->engine->GetStats().busy > 0 && s->engine->GetStats().busy > 0);
    CHECK(s->engine->GetStats().seqGaps == 0 && c->engine->GetStats().seqGaps == 0);
    std::printf("  sim busy back-pressure OK\n");
}

static void TestSimSeqGaps() {
    // Fixed drops of data packets in the middle of interleaved messages.
    {
        Sim sim;
        auto* s = sim.AddNode("S", ServerCfg());
        auto* c = sim.AddNode("C", ClientCfg());
        auto chans = sim.Connect(c, s, 20, 20);
        CHECK(sim.Run([&] { return Ready(s) && Ready(c); }));
        std::set<uint64_t> drop = {5, 6, 40, 41, 42, 300};
        chans.first->filter = [&](uint64_t i, Bytes&) { return drop.count(i) ? Action::Drop : Action::Deliver; };
        Exchange(sim, c, "S", s, {Pattern(3000, 1), Pattern(700, 2), Pattern(5, 3), Pattern(2000, 4)});
        CHECK(s->engine->GetStats().seqGaps >= 3);
        CHECK(s->engine->GetStats().resumesSent >= 3);
        CHECK(c->engine->GetStats().restarts >= 1);
    }
    // Random 0.5% loss in both directions (data, ACKs, RESUMEs alike); everything still arrives exactly once.
    for (uint32_t seed : {1u, 2u, 3u}) {
        Sim sim;
        sim.rng = seed * 7919;
        auto* s = sim.AddNode("S", ServerCfg());
        auto* c = sim.AddNode("C", ClientCfg());
        auto chans = sim.Connect(c, s, 182, 182, 4);
        CHECK(sim.Run([&] { return Ready(s) && Ready(c); }));
        auto lossy = [&sim](uint64_t, Bytes& b) {
            if (b[0] == 1) return Action::Deliver;  // keep the handshake out of it
            return sim.Rand() % 1000 < 5 ? Action::Drop : Action::Deliver;
        };
        chans.first->filter = lossy;
        chans.second->filter = lossy;
        std::vector<Bytes> msgs;
        for (int i = 0; i < 10; ++i) msgs.push_back(Pattern(static_cast<size_t>(i * i) * 397 + 3, i + seed));
        for (const auto& m : msgs) CHECK(s->engine->Send("C", m).code == SendOutcome::Code::Ok);
        Exchange(sim, c, "S", s, msgs);
        CHECK(sim.Run([&] { return Sim::Count(c, EventType::MessageReceived) == msgs.size(); }));
        CHECK(Sim::Count(c, EventType::MessageReceived) == msgs.size());
        CHECK(Sim::Count(s, EventType::MessageFailed) == 0 && Sim::Count(c, EventType::MessageFailed) == 0);
        const Stats ss = s->engine->GetStats(), cs = c->engine->GetStats();
        CHECK(ss.seqGaps + cs.seqGaps > 0);  // the loss really happened
        std::printf("    seed %u: gaps %llu/%llu, restarts %llu/%llu, ack timeouts %llu/%llu\n", seed,
                    static_cast<unsigned long long>(ss.seqGaps), static_cast<unsigned long long>(cs.seqGaps),
                    static_cast<unsigned long long>(ss.restarts), static_cast<unsigned long long>(cs.restarts),
                    static_cast<unsigned long long>(ss.ackTimeouts), static_cast<unsigned long long>(cs.ackTimeouts));
    }
    std::printf("  sim injected seq gaps OK\n");
}

static void TestSimDuplicates() {
    Sim sim;
    auto* s = sim.AddNode("S", ServerCfg());
    auto* c = sim.AddNode("C", ClientCfg());
    auto chans = sim.Connect(c, s, 244, 244);
    CHECK(sim.Run([&] { return Ready(s) && Ready(c); }));
    // Back-to-back duplicates (stack duplicate) of data, START and ACK packets.
    chans.first->filter = [](uint64_t i, Bytes&) { return i % 3 == 0 ? Action::Duplicate : Action::Deliver; };
    chans.second->filter = [](uint64_t, Bytes&) { return Action::Duplicate; };
    Exchange(sim, c, "S", s, {Pattern(10000, 1), Pattern(1, 2), Bytes(), Pattern(500, 3)});
    CHECK(s->engine->GetStats().duplicatesDropped > 0 && c->engine->GetStats().duplicatesDropped > 0);
    CHECK(s->engine->GetStats().seqGaps == 0);
    // A late re-delivery of an old packet (not back to back): seq gap, repaired, still exactly once.
    Bytes saved;
    int injected = 0;
    chans.first->filter = [&](uint64_t, Bytes& b) {
        if (b[0] == 3 && saved.empty()) saved = b;
        if (!saved.empty() && injected < 2 && b[0] == 3 && b != saved && (++injected, true)) {
            chans.first->q.push_back(saved);  // re-delivered before the current packet
        }
        return Action::Deliver;
    };
    chans.second->filter = nullptr;
    Exchange(sim, c, "S", s, {Pattern(8000, 4), Pattern(3000, 5)});
    CHECK(s->engine->GetStats().seqGaps > 0);
    std::printf("  sim duplicate packets OK\n");
}

static void TestSimDisconnect() {
    Sim sim;
    auto* s = sim.AddNode("S", ServerCfg());
    auto* c = sim.AddNode("C", ClientCfg());
    sim.Connect(c, s, 244, 244);
    CHECK(sim.Run([&] { return Ready(s) && Ready(c); }));
    const Bytes big = Pattern(1000000, 3);
    const uint32_t h = c->engine->Send("S", big).handle;
    const uint32_t h2 = s->engine->Send("C", big).handle;
    CHECK(sim.Run([&] {
        for (const auto* ev : Sim::All(s, EventType::Progress)) {
            if (ev->progressKind == ProgressKind::Receiving && ev->bytes >= 200000) return true;
        }
        return false;
    }));
    sim.Disconnect(c, s);
    CHECK(sim.Run([&] { return Sim::Count(c, EventType::PeerGone) == 1 && Sim::Count(s, EventType::PeerGone) == 1; }));
    const Event* f = Sim::All(c, EventType::MessageFailed).back();
    CHECK(f->handle == h && f->reason == FailReason::Disconnected);
    const Event* f2 = Sim::All(s, EventType::MessageFailed).back();
    CHECK(f2->handle == h2 && f2->reason == FailReason::Disconnected);
    CHECK(Sim::Count(s, EventType::MessageReceived) == 0 && Sim::Count(c, EventType::MessageReceived) == 0);
    CHECK(s->engine->Send("C", Bytes(5)).code == SendOutcome::Code::NotReady);
    // reconnect: new session (msgIds and seq restart), the app re-sends
    sim.Connect(c, s, 244, 244);
    CHECK(sim.Run([&] { return Sim::Count(s, EventType::PeerReady) == 2 && Sim::Count(c, EventType::PeerReady) == 2; }));
    Exchange(sim, c, "S", s, {big});
    CHECK(Sim::All(s, EventType::MessageReceived).back()->msgId == 0);
    std::printf("  sim disconnect mid-message OK\n");
}

static void TestSimInterleaving() {
    Sim sim;
    auto* s = sim.AddNode("S", ServerCfg());
    auto* c = sim.AddNode("C", ClientCfg());
    sim.Connect(c, s, 244, 244);
    CHECK(sim.Run([&] { return Ready(s) && Ready(c); }));
    const Bytes big = Pattern(2000000, 1);
    std::vector<Bytes> msgs = {big};
    CHECK(c->engine->Send("S", big).code == SendOutcome::Code::Ok);
    // Let the big one get going, then send small ones: they overtake it (completion order).
    CHECK(sim.Run([&] { return s->engine->GetStats().packetsReceived > 200; }));
    for (int i = 0; i < 3; ++i) CHECK(c->engine->Send("S", Pattern(30, i)).code == SendOutcome::Code::Ok);
    CHECK(sim.Run([&] { return Sim::Count(s, EventType::MessageReceived) == 4; }));
    auto got = Sim::All(s, EventType::MessageReceived);
    CHECK(got.back()->data == big);
    for (int i = 0; i < 3; ++i) CHECK(got[static_cast<size_t>(i)]->data == Pattern(30, i));
    std::printf("  sim interleaved small+large OK\n");
}

static void TestSimWindowFull() {
    Sim sim;
    Config sc = ServerCfg();
    sc.window = 1;
    auto* s = sim.AddNode("S", sc);
    auto* c = sim.AddNode("C", ClientCfg());
    sim.Connect(c, s, 244, 244);
    CHECK(sim.Run([&] { return Ready(s) && Ready(c); }));
    std::vector<Bytes> msgs;
    for (int i = 0; i < 6; ++i) msgs.push_back(Pattern(static_cast<size_t>(i) * 1000 + 10, i));
    size_t maxInFlight = 0;
    for (const auto& m : msgs) CHECK(c->engine->Send("S", m).code == SendOutcome::Code::Ok);
    CHECK(sim.Run([&] {
        PeerInfo info;
        if (c->engine->GetPeerInfo("S", info)) maxInFlight = std::max(maxInFlight, info.inFlight);
        return Sim::Count(s, EventType::MessageReceived) == msgs.size();
    }));
    CHECK(maxInFlight == 1);
    auto got = Sim::All(s, EventType::MessageReceived);
    for (size_t i = 0; i < msgs.size(); ++i) CHECK(got[i]->data == msgs[i]);  // window 1 = send order
    CHECK(s->engine->GetStats().rejected == 0);
    std::printf("  sim window full OK\n");
}

static void TestSimCrc() {
    Sim sim;
    auto* s = sim.AddNode("S", ServerCfg());
    auto* c = sim.AddNode("C", ClientCfg());
    auto chans = sim.Connect(c, s, 244, 244);
    CHECK(sim.Run([&] { return Ready(s) && Ready(c); }));
    // One corrupted payload byte: ACK 1, automatic resend, delivered intact once.
    bool corrupted = false;
    chans.first->filter = [&](uint64_t, Bytes& b) {
        if (!corrupted && b[0] == 3) {
            b[10] ^= 0x01;
            corrupted = true;
        }
        return Action::Deliver;
    };
    Exchange(sim, c, "S", s, {Pattern(4000, 9)});
    CHECK(c->engine->GetStats().crcRetries == 1);
    CHECK(s->engine->GetStats().rejected == 1);
    // Persistent corruption: fail with crc after 3 resends.
    chans.first->filter = [&](uint64_t, Bytes& b) {
        if (b[0] == 2 && b.size() > 12) b[12] ^= 0x80;
        return Action::Deliver;
    };
    const uint32_t h = c->engine->Send("S", Pattern(300, 1)).handle;
    CHECK(sim.Run([&] { return Sim::Count(c, EventType::MessageFailed) == 1; }));
    const Event* f = Sim::All(c, EventType::MessageFailed)[0];
    CHECK(f->handle == h && f->reason == FailReason::Crc && f->ackStatus == 1);
    CHECK(Sim::Count(s, EventType::MessageReceived) == 1);
    std::printf("  sim CRC mismatch OK\n");
}

static void TestSimOversize() {
    Sim sim;
    Config sc = ServerCfg();
    sc.maxMessage = 4096;
    auto* s = sim.AddNode("S", sc);
    auto* c = sim.AddNode("C", ClientCfg());
    sim.Connect(c, s, 244, 244);
    CHECK(sim.Run([&] { return Ready(s) && Ready(c); }));
    CHECK(c->engine->Send("S", Bytes(4097)).code == SendOutcome::Code::TooLarge);
    Exchange(sim, c, "S", s, {Pattern(4096, 1)});
    std::printf("  sim oversize rejection OK\n");
}

static void TestSimLostAck() {
    Sim sim;
    auto* s = sim.AddNode("S", ServerCfg());
    auto* c = sim.AddNode("C", ClientCfg());
    auto chans = sim.Connect(c, s, 244, 244);
    CHECK(sim.Run([&] { return Ready(s) && Ready(c); }));
    int acksDropped = 0;
    chans.second->filter = [&](uint64_t, Bytes& b) {
        if (b[0] == 4 && acksDropped < 2) {
            ++acksDropped;
            return Action::Drop;
        }
        return Action::Deliver;
    };
    const uint64_t t0 = sim.now;
    Exchange(sim, c, "S", s, {Pattern(1500, 1)});
    CHECK(Sim::Count(s, EventType::MessageReceived) == 1);  // not delivered twice
    CHECK(c->engine->GetStats().ackTimeouts == 2);
    CHECK(s->engine->GetStats().duplicatesAcked >= 1);
    CHECK(sim.now - t0 >= 20000);
    std::printf("  sim lost ACK -> timeout resend, no double delivery OK\n");
}

static void TestSimMultiplePeers() {
    Sim sim;
    auto* s = sim.AddNode("S", ServerCfg());
    auto* c1 = sim.AddNode("C1", ClientCfg(20));
    auto* c2 = sim.AddNode("C2", ClientCfg(182));
    Config mcu = ClientCfg(244);
    mcu.window = 1;
    mcu.maxMessage = 4096;
    auto* c3 = sim.AddNode("C3", mcu);
    sim.Connect(c1, s, 20, 509);
    sim.Connect(c2, s, 182, 509);
    sim.Connect(c3, s, 244, 509);
    CHECK(sim.Run([&] { return Sim::Count(s, EventType::PeerReady) == 3 && Ready(c1) && Ready(c2) && Ready(c3); }));
    PeerInfo i1, i2, i3;
    CHECK(s->engine->GetPeerInfo("C1", i1) && i1.txPacketSize == 20);
    CHECK(s->engine->GetPeerInfo("C2", i2) && i2.txPacketSize == 182);
    CHECK(s->engine->GetPeerInfo("C3", i3) && i3.txPacketSize == 244 && i3.peer.window == 1 &&
          i3.peer.maxMessage == 4096);
    CHECK(s->engine->Send("C3", Bytes(5000)).code == SendOutcome::Code::TooLarge);
    std::map<std::string, Bytes> toPeer = {{"C1", Pattern(3000, 1)}, {"C2", Pattern(30000, 2)}, {"C3", Pattern(4096, 3)}};
    std::map<std::string, Bytes> fromPeer = {{"C1", Pattern(2000, 4)}, {"C2", Pattern(20000, 5)}, {"C3", Pattern(1000, 6)}};
    for (auto& kv : toPeer) CHECK(s->engine->Send(kv.first, kv.second).code == SendOutcome::Code::Ok);
    CHECK(c1->engine->Send("S", fromPeer["C1"]).code == SendOutcome::Code::Ok);
    CHECK(c2->engine->Send("S", fromPeer["C2"]).code == SendOutcome::Code::Ok);
    CHECK(c3->engine->Send("S", fromPeer["C3"]).code == SendOutcome::Code::Ok);
    CHECK(sim.Run([&] {
        return Sim::Count(s, EventType::MessageReceived) == 3 && Sim::Count(s, EventType::MessageSent) == 3 &&
               Sim::Count(c1, EventType::MessageReceived) == 1 && Sim::Count(c2, EventType::MessageReceived) == 1 &&
               Sim::Count(c3, EventType::MessageReceived) == 1;
    }));
    for (const auto* ev : Sim::All(s, EventType::MessageReceived)) CHECK(ev->data == fromPeer[ev->peer]);
    CHECK(Sim::All(c1, EventType::MessageReceived)[0]->data == toPeer["C1"]);
    CHECK(Sim::All(c2, EventType::MessageReceived)[0]->data == toPeer["C2"]);
    CHECK(Sim::All(c3, EventType::MessageReceived)[0]->data == toPeer["C3"]);
    // One peer leaving does not disturb the others.
    sim.Disconnect(c2, s);
    Exchange(sim, c1, "S", s, {Pattern(777, 8)});
    CHECK(Sim::Count(s, EventType::PeerGone) == 1 && Sim::All(s, EventType::PeerGone)[0]->peer == "C2");
    std::printf("  sim multiple peers OK\n");
}

static void TestSimResetPeer() {
    Sim sim;
    auto* s = sim.AddNode("S", ServerCfg());
    auto* c = sim.AddNode("C", ClientCfg());
    sim.Connect(c, s, 244, 244);
    CHECK(sim.Run([&] { return Ready(s) && Ready(c); }));
    const uint32_t h = s->engine->Send("C", Pattern(500000, 1)).handle;
    CHECK(sim.Run([&] { return c->engine->GetStats().packetsReceived > 100; }));
    c->engine->ResetPeer("S");  // e.g. the MTU changed on the client
    CHECK(sim.Run([&] { return Sim::Count(c, EventType::PeerReady) == 2 && Sim::Count(s, EventType::PeerReady) == 2; }));
    const Event* f = Sim::All(s, EventType::MessageFailed).back();
    CHECK(f->handle == h && f->reason == FailReason::Reset);
    Exchange(sim, s, "C", c, {Pattern(5000, 2)});
    Exchange(sim, c, "S", s, {Pattern(5000, 3)});
    std::printf("  sim reset OK\n");
}

// ---------------------------------------------------------------------------------------------------------
// 4. Threaded smoke test
// ---------------------------------------------------------------------------------------------------------

class ThreadLink {
public:
    struct End : ITransport {
        ThreadLink* link = nullptr;
        int side = 0;
        TransportResult SendPacket(const PeerId&, const uint8_t* data, size_t len) override {
            std::lock_guard<std::mutex> lk(link->mu_);
            auto& q = link->q_[side];
            if (q.size() >= 8) {
                link->wantReady_[side] = true;
                return TransportResult::Busy;
            }
            q.emplace_back(data, data + len);
            link->cv_.notify_one();
            return TransportResult::Sent;
        }
    };

    End ends[2];
    Engine* engines[2] = {nullptr, nullptr};
    PeerId names[2];  // names[i] = how side i names the other side

    ThreadLink() {
        for (int i = 0; i < 2; ++i) {
            ends[i].link = this;
            ends[i].side = i;
        }
    }
    void Start() {
        thread_ = std::thread([this] { Loop(); });
    }
    void Stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

private:
    void Loop() {
        std::unique_lock<std::mutex> lk(mu_);
        for (;;) {
            cv_.wait_for(lk, std::chrono::milliseconds(5));
            if (stop_) return;
            for (int side = 0; side < 2; ++side) {
                std::deque<Bytes> batch;
                batch.swap(q_[side]);
                const bool ready = wantReady_[side];
                wantReady_[side] = false;
                lk.unlock();
                const int other = 1 - side;
                for (auto& b : batch) engines[other]->OnPacket(names[other], b.data(), b.size());
                if (ready || !batch.empty()) engines[side]->OnTransportReady(names[side]);
                lk.lock();
            }
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Bytes> q_[2];
    bool wantReady_[2] = {false, false};
    bool stop_ = false;
    std::thread thread_;
};

static void TestThreaded() {
    ThreadLink link;
    Engine server(link.ends[0], ServerCfg());
    Engine client(link.ends[1], ClientCfg());
    link.engines[0] = &server;
    link.engines[1] = &client;
    link.names[0] = "C";
    link.names[1] = "S";

    std::mutex mu;
    std::condition_variable cv;
    std::vector<Bytes> atServer, atClient;
    int ready = 0, sentS = 0, sentC = 0;
    server.SetEventHandler([&](const Event& ev) {
        std::lock_guard<std::mutex> lk(mu);
        if (ev.type == EventType::PeerReady) ++ready;
        if (ev.type == EventType::MessageReceived) atServer.push_back(ev.data);
        if (ev.type == EventType::MessageSent) ++sentS;
        cv.notify_all();
    });
    client.SetEventHandler([&](const Event& ev) {
        // A handler may call back into the engine.
        PeerInfo info;
        client.GetPeerInfo("S", info);
        std::lock_guard<std::mutex> lk(mu);
        if (ev.type == EventType::PeerReady) ++ready;
        if (ev.type == EventType::MessageReceived) atClient.push_back(ev.data);
        if (ev.type == EventType::MessageSent) ++sentC;
        cv.notify_all();
    });
    server.Start();
    client.Start();
    link.Start();
    server.SetPeerOsLimit("C", 244);
    client.SetPeerOsLimit("S", 182);
    server.OnPeerConnected("C");
    server.OnPeerSubscribed("C");
    client.OnPeerConnected("S");
    client.OnPeerSubscribed("S");
    {
        std::unique_lock<std::mutex> lk(mu);
        CHECK(cv.wait_for(lk, std::chrono::seconds(10), [&] { return ready == 2; }));
    }
    std::vector<Bytes> msgs = {Pattern(300000, 1), Bytes(), Pattern(1, 2), Pattern(5000, 3)};
    std::thread t1([&] {
        for (const auto& m : msgs) CHECK(client.Send("S", m).code == SendOutcome::Code::Ok);
    });
    std::thread t2([&] {
        for (const auto& m : msgs) CHECK(server.Send("C", m).code == SendOutcome::Code::Ok);
    });
    t1.join();
    t2.join();
    {
        std::unique_lock<std::mutex> lk(mu);
        CHECK(cv.wait_for(lk, std::chrono::seconds(60), [&] {
            return atServer.size() == msgs.size() && atClient.size() == msgs.size() && sentS == 4 && sentC == 4;
        }));
        std::multiset<Bytes> want(msgs.begin(), msgs.end());
        CHECK(std::multiset<Bytes>(atServer.begin(), atServer.end()) == want);
        CHECK(std::multiset<Bytes>(atClient.begin(), atClient.end()) == want);
    }
    link.Stop();
    server.Shutdown();
    client.Shutdown();
    CHECK(server.Send("C", Bytes(1)).code == SendOutcome::Code::Stopped);
    std::printf("  threaded engines OK\n");
}

int main() {
    std::printf("BleLink tests\n");
    const Json v = LoadVectors();
    TestCodecVectors(v);
    TestCodecEdges();
    TestScriptedHandshake();
    TestScriptedInterleavingVector(v);
    TestScriptedResumeVector(v);
    TestScriptedReceiverLimits();
    TestScriptedReceiverGapAndIdle();
    TestScriptedSender();
    TestScriptedResetAndClient();
    TestSimPacketSizes();
    TestSimBackPressure();
    TestSimSeqGaps();
    TestSimDuplicates();
    TestSimDisconnect();
    TestSimInterleaving();
    TestSimWindowFull();
    TestSimCrc();
    TestSimOversize();
    TestSimLostAck();
    TestSimMultiplePeers();
    TestSimResetPeer();
    TestSimHuge();
    TestThreaded();
    std::printf("BleLink tests: all passed\n");
    return 0;
}
