// BleLinkCodec.h - BleLink protocol v1 packet codec and CRC-32 (docs/PROTOCOL.md).
// Pure C++17, no OS headers, no allocation except into caller-owned vectors.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace BleServer {
namespace BleLink {

constexpr uint8_t kProtocolVersion = 1;
constexpr uint16_t kMinPacketSize = 20;   // ATT_MTU 23 - 3
constexpr uint16_t kMaxPacketSize = 512;  // ATT attribute value limit
constexpr uint8_t kDefaultTimeoutUnits = 40;  // 40 * 0.25 s = 10 s
constexpr uint32_t kTimeoutUnitMs = 250;
constexpr uint32_t kNoMessageLimit = 0xFFFFFFFFu;
constexpr uint32_t kProgressStep = 65536;  // PROGRESS granularity (section 16)

constexpr uint8_t kFlagProgress = 0x01;   // HELLO flags bit 0
constexpr uint8_t kFlagStreaming = 0x02;  // HELLO flags bit 1 (informational)

enum class PacketType : uint8_t {
    Hello = 0x01,
    MsgStart = 0x02,
    MsgData = 0x03,
    Ack = 0x04,
    Resume = 0x05,
    Cancel = 0x06,
    Progress = 0x07,
    MsgResend = 0x08,
};

// Fixed sizes / header sizes (header = bytes before the payload).
constexpr size_t kHelloSize = 12;
constexpr size_t kInfoSize = 10;
constexpr size_t kStartHeader = 12;
constexpr size_t kDataHeader = 4;
constexpr size_t kResendHeader = 8;
constexpr size_t kAckSize = 5;
constexpr size_t kResumeSize = 8;
constexpr size_t kCancelSize = 5;
constexpr size_t kProgressSize = 8;

enum class AckStatus : uint8_t {
    Ok = 0,
    CrcMismatch = 1,
    TooLarge = 2,
    NoMemory = 3,
    Rejected = 4,
};

enum class CancelReason : uint8_t {
    Application = 0,
    GaveUp = 1,
    Shutdown = 2,
};

// HELLO body: every field describes the HELLO's sender as a receiver (section 4.1).
struct Hello {
    uint8_t version = kProtocolVersion;
    uint16_t maxPacket = kMinPacketSize;
    uint32_t maxMessage = 0;
    uint8_t window = 1;
    uint8_t flags = 0;
    uint8_t timeout = 0;  // 0.25 s units, 0 = default 40
};

// CRC-32/IEEE 802.3 (zlib). Chainable like zlib's crc32(): Crc32(Crc32(0, a), b) == Crc32(0, a+b).
uint32_t Crc32(uint32_t crc, const uint8_t* data, size_t len);

// Clamp a raw maxPacket / OS limit into 20..512.
uint16_t ClampPacketSize(uint32_t value);
// Timeout field -> milliseconds (0 -> 40 units).
uint32_t TimeoutUnitsToMs(uint8_t units);

// Decoded packet view. Fields not used by the packet's type are zero. payload points into the input buffer.
struct Packet {
    uint8_t type = 0;
    uint8_t seq = 0;
    uint16_t msgId = 0;
    uint32_t totalLen = 0;     // MSG_START
    uint32_t crc = 0;          // MSG_START
    uint32_t offset = 0;       // RESUME, MSG_RESEND
    uint32_t bytesReceived = 0;  // PROGRESS
    uint8_t status = 0;        // ACK
    uint8_t reason = 0;        // CANCEL
    Hello hello;               // HELLO (raw values, not clamped)
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;
};

enum class DecodeResult {
    Ok,
    Empty,        // fewer than 2 bytes: no header at all
    UnknownType,  // header valid, type unknown: ignore (seq still counts)
    TooShort,     // known type, shorter than its fixed layout: ignore
};

// Decodes one packet. type and seq are filled whenever len >= 2.
DecodeResult Decode(const uint8_t* data, size_t len, Packet& out);

// Encoders. Each one replaces the content of `out` with exactly one packet.
void EncodeHello(std::vector<uint8_t>& out, uint8_t seq, const Hello& hello);
void EncodeMsgStart(std::vector<uint8_t>& out, uint8_t seq, uint16_t msgId, uint32_t totalLen, uint32_t crc,
                    const uint8_t* payload, size_t n);
void EncodeMsgData(std::vector<uint8_t>& out, uint8_t seq, uint16_t msgId, const uint8_t* payload, size_t n);
void EncodeMsgResend(std::vector<uint8_t>& out, uint8_t seq, uint16_t msgId, uint32_t offset,
                     const uint8_t* payload, size_t n);
void EncodeAck(std::vector<uint8_t>& out, uint8_t seq, uint16_t msgId, uint8_t status);
void EncodeResume(std::vector<uint8_t>& out, uint8_t seq, uint16_t msgId, uint32_t offset);
void EncodeCancel(std::vector<uint8_t>& out, uint8_t seq, uint16_t msgId, uint8_t reason);
void EncodeProgress(std::vector<uint8_t>& out, uint8_t seq, uint16_t msgId, uint32_t bytesReceived);

// INFO characteristic value: HELLO body without type/seq (10 bytes).
void EncodeInfo(std::vector<uint8_t>& out, const Hello& hello);
bool DecodeInfo(const uint8_t* data, size_t len, Hello& out);

// Payload capacity per packet type at a given packetSize (packetSize must be >= 20).
inline size_t StartCapacity(uint16_t packetSize) { return packetSize - kStartHeader; }
inline size_t DataCapacity(uint16_t packetSize) { return packetSize - kDataHeader; }
inline size_t ResendCapacity(uint16_t packetSize) { return packetSize - kResendHeader; }

// Reference framing of a whole message (section 2.4): MSG_START then MSG_DATA, seq firstSeq, firstSeq+1, ...
// The engine produces the same bytes; used by tests and tools.
std::vector<std::vector<uint8_t>> SplitMessage(uint16_t packetSize, uint16_t msgId, uint8_t firstSeq,
                                               const uint8_t* data, size_t len);

}  // namespace BleLink
}  // namespace BleServer
