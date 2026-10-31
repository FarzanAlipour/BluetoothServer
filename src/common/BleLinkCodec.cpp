// BleLinkCodec.cpp - BleLink protocol v1 packet codec (docs/PROTOCOL.md sections 2 and 4).
#include "BleLinkCodec.h"

#include <algorithm>
#include <array>

namespace BleServer {
namespace BleLink {

namespace {

std::array<uint32_t, 256> MakeCrcTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

const std::array<uint32_t, 256>& CrcTable() {
    static const std::array<uint32_t, 256> table = MakeCrcTable();
    return table;
}

inline void Put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

inline void Put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

inline uint16_t Get16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t Get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline void Begin(std::vector<uint8_t>& out, PacketType type, uint8_t seq, size_t reserve) {
    out.clear();
    out.reserve(reserve);
    out.push_back(static_cast<uint8_t>(type));
    out.push_back(seq);
}

inline void PutBody(std::vector<uint8_t>& out, const Hello& h) {
    out.push_back(h.version);
    Put16(out, h.maxPacket);
    Put32(out, h.maxMessage);
    out.push_back(h.window);
    out.push_back(h.flags);
    out.push_back(h.timeout);
}

inline void GetBody(const uint8_t* p, Hello& h) {
    h.version = p[0];
    h.maxPacket = Get16(p + 1);
    h.maxMessage = Get32(p + 3);
    h.window = p[7];
    h.flags = p[8];
    h.timeout = p[9];
}

}  // namespace

uint32_t Crc32(uint32_t crc, const uint8_t* data, size_t len) {
    const auto& table = CrcTable();
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

uint16_t ClampPacketSize(uint32_t value) {
    if (value < kMinPacketSize) return kMinPacketSize;
    if (value > kMaxPacketSize) return kMaxPacketSize;
    return static_cast<uint16_t>(value);
}

uint32_t TimeoutUnitsToMs(uint8_t units) {
    return static_cast<uint32_t>(units == 0 ? kDefaultTimeoutUnits : units) * kTimeoutUnitMs;
}

DecodeResult Decode(const uint8_t* data, size_t len, Packet& out) {
    out = Packet{};
    if (data == nullptr || len < 2) return DecodeResult::Empty;
    out.type = data[0];
    out.seq = data[1];
    const uint8_t* b = data + 2;
    switch (static_cast<PacketType>(out.type)) {
    case PacketType::Hello:
        if (len < kHelloSize) return DecodeResult::TooShort;
        GetBody(b, out.hello);
        return DecodeResult::Ok;
    case PacketType::MsgStart:
        if (len < kStartHeader) return DecodeResult::TooShort;
        out.msgId = Get16(b);
        out.totalLen = Get32(b + 2);
        out.crc = Get32(b + 6);
        out.payload = data + kStartHeader;
        out.payloadLen = len - kStartHeader;
        return DecodeResult::Ok;
    case PacketType::MsgData:
        if (len < kDataHeader) return DecodeResult::TooShort;
        out.msgId = Get16(b);
        out.payload = data + kDataHeader;
        out.payloadLen = len - kDataHeader;
        return DecodeResult::Ok;
    case PacketType::Ack:
        if (len < kAckSize) return DecodeResult::TooShort;
        out.msgId = Get16(b);
        out.status = b[2];
        return DecodeResult::Ok;
    case PacketType::Resume:
        if (len < kResumeSize) return DecodeResult::TooShort;
        out.msgId = Get16(b);
        out.offset = Get32(b + 2);
        return DecodeResult::Ok;
    case PacketType::Cancel:
        if (len < kCancelSize) return DecodeResult::TooShort;
        out.msgId = Get16(b);
        out.reason = b[2];
        return DecodeResult::Ok;
    case PacketType::Progress:
        if (len < kProgressSize) return DecodeResult::TooShort;
        out.msgId = Get16(b);
        out.bytesReceived = Get32(b + 2);
        return DecodeResult::Ok;
    case PacketType::MsgResend:
        if (len < kResendHeader) return DecodeResult::TooShort;
        out.msgId = Get16(b);
        out.offset = Get32(b + 2);
        out.payload = data + kResendHeader;
        out.payloadLen = len - kResendHeader;
        return DecodeResult::Ok;
    }
    return DecodeResult::UnknownType;
}

void EncodeHello(std::vector<uint8_t>& out, uint8_t seq, const Hello& hello) {
    Begin(out, PacketType::Hello, seq, kHelloSize);
    PutBody(out, hello);
}

void EncodeMsgStart(std::vector<uint8_t>& out, uint8_t seq, uint16_t msgId, uint32_t totalLen, uint32_t crc,
                    const uint8_t* payload, size_t n) {
    Begin(out, PacketType::MsgStart, seq, kStartHeader + n);
    Put16(out, msgId);
    Put32(out, totalLen);
    Put32(out, crc);
    if (n > 0) out.insert(out.end(), payload, payload + n);
}

void EncodeMsgData(std::vector<uint8_t>& out, uint8_t seq, uint16_t msgId, const uint8_t* payload, size_t n) {
    Begin(out, PacketType::MsgData, seq, kDataHeader + n);
    Put16(out, msgId);
    if (n > 0) out.insert(out.end(), payload, payload + n);
}

void EncodeMsgResend(std::vector<uint8_t>& out, uint8_t seq, uint16_t msgId, uint32_t offset,
                     const uint8_t* payload, size_t n) {
    Begin(out, PacketType::MsgResend, seq, kResendHeader + n);
    Put16(out, msgId);
    Put32(out, offset);
    if (n > 0) out.insert(out.end(), payload, payload + n);
}

void EncodeAck(std::vector<uint8_t>& out, uint8_t seq, uint16_t msgId, uint8_t status) {
    Begin(out, PacketType::Ack, seq, kAckSize);
    Put16(out, msgId);
    out.push_back(status);
}

void EncodeResume(std::vector<uint8_t>& out, uint8_t seq, uint16_t msgId, uint32_t offset) {
    Begin(out, PacketType::Resume, seq, kResumeSize);
    Put16(out, msgId);
    Put32(out, offset);
}

void EncodeCancel(std::vector<uint8_t>& out, uint8_t seq, uint16_t msgId, uint8_t reason) {
    Begin(out, PacketType::Cancel, seq, kCancelSize);
    Put16(out, msgId);
    out.push_back(reason);
}

void EncodeProgress(std::vector<uint8_t>& out, uint8_t seq, uint16_t msgId, uint32_t bytesReceived) {
    Begin(out, PacketType::Progress, seq, kProgressSize);
    Put16(out, msgId);
    Put32(out, bytesReceived);
}

void EncodeInfo(std::vector<uint8_t>& out, const Hello& hello) {
    out.clear();
    out.reserve(kInfoSize);
    PutBody(out, hello);
}

bool DecodeInfo(const uint8_t* data, size_t len, Hello& out) {
    if (data == nullptr || len < kInfoSize) return false;
    GetBody(data, out);
    return true;
}

std::vector<std::vector<uint8_t>> SplitMessage(uint16_t packetSize, uint16_t msgId, uint8_t firstSeq,
                                               const uint8_t* data, size_t len) {
    packetSize = ClampPacketSize(packetSize);
    std::vector<std::vector<uint8_t>> packets;
    const uint32_t crc = Crc32(0, data, len);
    uint8_t seq = firstSeq;
    size_t n = std::min(len, StartCapacity(packetSize));
    packets.emplace_back();
    EncodeMsgStart(packets.back(), seq++, msgId, static_cast<uint32_t>(len), crc, data, n);
    size_t offset = n;
    while (offset < len) {
        n = std::min(len - offset, DataCapacity(packetSize));
        packets.emplace_back();
        EncodeMsgData(packets.back(), seq++, msgId, data + offset, n);
        offset += n;
    }
    return packets;
}

}  // namespace BleLink
}  // namespace BleServer
