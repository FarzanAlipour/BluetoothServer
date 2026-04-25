#include "BleMessageChunker.h"

#include <cassert>
#include <cstdint>
#include <map>
#include <vector>

using namespace BleServer;

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

int main() {
    TestRoundTrip();
    TestAck();
    TestHandlerCallbacks();
    return 0;
}
