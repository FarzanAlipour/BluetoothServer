#include "BleMessageChunker.h"

#include <cassert>
#include <cstdint>
#include <map>
#include <vector>

using namespace BleServer;

/* ------------------------------------------------------------------ */
/* Existing tests (round-trip, ack, handler callbacks)                */
/* ------------------------------------------------------------------ */

static void TestRoundTrip() {
    std::vector<uint8_t> message(1024);
    for (size_t i = 0; i < message.size(); ++i) {
        message[i] = static_cast<uint8_t>(i & 0xFF);
    }

    auto chunks = BleMessageChunker::CreateChunks(message);
    assert(chunks.size() > 1);

    std::map<uint32_t, std::vector<std::vector<uint8_t>>> incoming;
    std::vector<uint8_t> complete;
    bool completed = false;

    // Deliver out of order to validate index-based reassembly.
    for (auto it = chunks.rbegin(); it != chunks.rend(); ++it) {
        completed = BleMessageChunker::ProcessChunk(*it, incoming, complete);
    }

    assert(completed);
    assert(complete == message);
}

static void TestAck() {
    auto chunks = BleMessageChunker::CreateChunks({1, 2, 3});
    assert(chunks.size() == 1);

    const uint32_t messageId = BleMessageChunker::GetMessageId(chunks[0]);
    auto ack = BleMessageChunker::CreateAck(messageId, 0);

    assert(BleMessageChunker::IsValidChunk(ack));
    assert(BleMessageChunker::IsAck(ack));
    assert(BleMessageChunker::GetMessageId(ack) == messageId);
    assert(BleMessageChunker::GetChunkIndex(ack) == 0);
}

static void TestHandlerCallbacks() {
    ChunkedMessageHandler handler;
    std::vector<std::vector<uint8_t>> sentChunks;
    const std::vector<uint8_t> payload = {'h', 'e', 'l', 'l', 'o'};

    const uint32_t messageId = handler.SendMessage(payload, [&](const std::vector<uint8_t>& chunk) {
        sentChunks.push_back(chunk);
    });
    assert(messageId != 0);
    assert(!sentChunks.empty());

    bool completed = false;
    handler.ProcessReceivedData(sentChunks.front(), [&](uint32_t receivedId, const std::vector<uint8_t>& message) {
        assert(receivedId == messageId);
        assert(message == payload);
        completed = true;
    });
    assert(completed);

    bool acked = false;
    handler.SetAckCallback([&](uint32_t ackMessageId, uint32_t chunkIndex) {
        assert(ackMessageId == messageId);
        assert(chunkIndex == 0);
        acked = true;
    });
    handler.ProcessReceivedData(BleMessageChunker::CreateAck(messageId, 0), nullptr);
    assert(acked);
}

/* ------------------------------------------------------------------ */
/* New edge-case tests (T4)                                           */
/* ------------------------------------------------------------------ */

static void TestEmptyMessage() {
    // Empty payload → single chunk with zero-length payload section
    std::vector<uint8_t> empty;
    auto chunks = BleMessageChunker::CreateChunks(empty);
    assert(chunks.size() == 1);
    assert(chunks[0].size() == BleMessageChunker::HEADER_SIZE);
}

static void TestSingleByte() {
    std::vector<uint8_t> msg = {0x42};
    auto chunks = BleMessageChunker::CreateChunks(msg);
    assert(chunks.size() == 1);

    std::map<uint32_t, std::vector<std::vector<uint8_t>>> incoming;
    std::vector<uint8_t> complete;
    bool done = BleMessageChunker::ProcessChunk(chunks[0], incoming, complete);
    assert(done);
    assert(complete == msg);
}

static void TestExactPayloadBoundary() {
    // Exactly MAX_PAYLOAD_SIZE bytes → should still be 1 chunk
    std::vector<uint8_t> msg(BleMessageChunker::MAX_PAYLOAD_SIZE, 0xAA);
    auto chunks = BleMessageChunker::CreateChunks(msg);
    assert(chunks.size() == 1);

    std::map<uint32_t, std::vector<std::vector<uint8_t>>> incoming;
    std::vector<uint8_t> complete;
    bool done = BleMessageChunker::ProcessChunk(chunks[0], incoming, complete);
    assert(done);
    assert(complete == msg);
}

static void TestJustOverBoundary() {
    // MAX_PAYLOAD_SIZE + 1 → should split into 2 chunks
    std::vector<uint8_t> msg(BleMessageChunker::MAX_PAYLOAD_SIZE + 1, 0xBB);
    auto chunks = BleMessageChunker::CreateChunks(msg);
    assert(chunks.size() == 2);

    std::map<uint32_t, std::vector<std::vector<uint8_t>>> incoming;
    std::vector<uint8_t> complete;
    bool done = false;
    for (const auto& c : chunks) {
        done = BleMessageChunker::ProcessChunk(c, incoming, complete);
    }
    assert(done);
    assert(complete == msg);
}

static void TestOversizedMessageRejected() {
    // Larger than MAX_MESSAGE_SIZE → empty chunks vector
    std::vector<uint8_t> huge(BleMessageChunker::MAX_MESSAGE_SIZE + 1, 0x00);
    auto chunks = BleMessageChunker::CreateChunks(huge);
    assert(chunks.empty());
}

static void TestInvalidChunk() {
    std::vector<uint8_t> tooShort = {0x01};   // less than HEADER_SIZE
    assert(!BleMessageChunker::IsValidChunk(tooShort));

    std::vector<uint8_t> badType(13, 0x01);  // unknown type byte
    badType[0] = 0xFF;

    // ACKs have totalChunks == 0; data chunks have totalChunks > 0.

    std::vector<uint8_t> badChunk(13, 0);
    badChunk[0] = BleMessageChunker::DATA_CHUNK;
    assert(!BleMessageChunker::IsValidChunk(badChunk)); // totalChunks==0 invalid for data
}

static void TestMtMrUConstants() {
    // Verify the new MTU/MRU constants are reasonable.
    assert(BleMessageChunker::BLE_MTU == 23);
    assert(BleMessageChunker::BLE_MRU == 517);
}

static void TestHandlerCleanupTimedOut() {
    ChunkedMessageHandler handler;
    std::vector<std::vector<uint8_t>> sent;
    handler.SendMessage({1, 2, 3}, [&](const std::vector<uint8_t>& chunk) {
        sent.push_back(chunk);
    });

    size_t out, in;
    handler.GetStatistics(out, in);
    assert(out == 1);

    // Timeout immediately — should remove the outgoing message.
    size_t removed = handler.CleanupTimedOut(0);
    assert(removed == 1);

    handler.GetStatistics(out, in);
    assert(out == 0);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main() {
    TestRoundTrip();
    TestAck();
    TestHandlerCallbacks();
    TestEmptyMessage();
    TestSingleByte();
    TestExactPayloadBoundary();
    TestJustOverBoundary();
    TestOversizedMessageRejected();
    TestInvalidChunk();
    TestMtMrUConstants();
    TestHandlerCleanupTimedOut();
    return 0;
}
