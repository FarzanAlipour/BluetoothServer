// BleMessageChunker.cpp - Message chunking implementation
#include "BleMessageChunker.h"
#include <atomic>
#include <algorithm>
#include <chrono>

namespace BleServer {

    // Message Chunker Implementation
    class MessageChunkerImpl {
    public:
        std::atomic<uint32_t> messageIdCounter{ 0 };

        uint32_t GenerateMessageId() {
            return ++messageIdCounter;
        }
    };

    static MessageChunkerImpl g_chunker;

    uint32_t BleMessageChunker::GenerateMessageId() {
        return g_chunker.GenerateMessageId();
    }

    std::vector<std::vector<uint8_t>> BleMessageChunker::CreateChunks(const std::vector<uint8_t>& message) {
        std::vector<std::vector<uint8_t>> chunks;

        uint32_t messageId = GenerateMessageId();
        uint32_t totalSize = static_cast<uint32_t>(message.size());
        if (message.size() > MAX_MESSAGE_SIZE) {
            return chunks;
        }

        uint32_t totalChunks = static_cast<uint32_t>((totalSize + MAX_PAYLOAD_SIZE - 1) / MAX_PAYLOAD_SIZE);
        if (totalChunks == 0) {
            totalChunks = 1;
        }

        for (uint32_t i = 0; i < totalChunks; ++i) {
            std::vector<uint8_t> chunk;
            chunk.reserve(CHUNK_SIZE);

            // Header
            chunk.push_back(0x01); // Data chunk type

            // Message ID (4 bytes, little-endian)
            chunk.push_back(messageId & 0xFF);
            chunk.push_back((messageId >> 8) & 0xFF);
            chunk.push_back((messageId >> 16) & 0xFF);
            chunk.push_back((messageId >> 24) & 0xFF);

            // Chunk index (4 bytes, little-endian)
            chunk.push_back(i & 0xFF);
            chunk.push_back((i >> 8) & 0xFF);
            chunk.push_back((i >> 16) & 0xFF);
            chunk.push_back((i >> 24) & 0xFF);

            // Total chunks (4 bytes, little-endian)
            chunk.push_back(totalChunks & 0xFF);
            chunk.push_back((totalChunks >> 8) & 0xFF);
            chunk.push_back((totalChunks >> 16) & 0xFF);
            chunk.push_back((totalChunks >> 24) & 0xFF);

            // Payload
            size_t offset = i * MAX_PAYLOAD_SIZE;
            size_t payloadSize = offset < message.size()
                ? std::min(MAX_PAYLOAD_SIZE, message.size() - offset)
                : 0;

            chunk.insert(chunk.end(),
                message.begin() + offset,
                message.begin() + offset + payloadSize);

            chunks.push_back(std::move(chunk));
        }

        return chunks;
    }

    bool BleMessageChunker::ProcessChunk(const std::vector<uint8_t>& chunk,
        std::map<uint32_t, std::vector<std::vector<uint8_t>>>& incomingMessages,
        std::vector<uint8_t>& completeMessage) {
        if (!IsValidChunk(chunk) || IsAck(chunk)) {
            return false;
        }

        // Parse header
        uint8_t type = chunk[0];

        if (type != 0x01) {
            // Unknown chunk type
            return false;
        }

        // Extract header fields
        uint32_t messageId = chunk[1] | (chunk[2] << 8) | (chunk[3] << 16) | (chunk[4] << 24);
        uint32_t chunkIndex = chunk[5] | (chunk[6] << 8) | (chunk[7] << 16) | (chunk[8] << 24);
        uint32_t totalChunks = chunk[9] | (chunk[10] << 8) | (chunk[11] << 16) | (chunk[12] << 24);

        // Validate chunk index
        if (chunkIndex >= totalChunks) {
            return false;
        }

        // Initialize message buffer if needed
        if (incomingMessages.find(messageId) == incomingMessages.end()) {
            incomingMessages[messageId].resize(totalChunks);
        }

        // Store chunk payload
        auto& messageChunks = incomingMessages[messageId];
        if (messageChunks.size() != totalChunks) {
            return false;
        }
        messageChunks[chunkIndex] = std::vector<uint8_t>(chunk.begin() + HEADER_SIZE, chunk.end());

        // Check if all chunks received
        bool complete = true;
        size_t totalSize = 0;
        for (size_t i = 0; i < messageChunks.size(); ++i) {
            const auto& msgChunk = messageChunks[i];
            if (msgChunk.empty()) {
                const bool emptySingleChunkMessage = totalChunks == 1 && i == chunkIndex && chunk.size() == HEADER_SIZE;
                if (!emptySingleChunkMessage) {
                    complete = false;
                    break;
                }
            }
            totalSize += msgChunk.size();
        }

        if (complete) {
            // Assemble complete message
            completeMessage.clear();
            completeMessage.reserve(totalSize);

            for (const auto& msgChunk : messageChunks) {
                completeMessage.insert(completeMessage.end(), msgChunk.begin(), msgChunk.end());
            }

            // Clean up
            incomingMessages.erase(messageId);
            return true;
        }

        return false;
    }

    std::vector<uint8_t> BleMessageChunker::CreateAck(uint32_t messageId, uint32_t chunkIndex) {
        std::vector<uint8_t> ack(HEADER_SIZE, 0);

        ack[0] = 0x02; // ACK type

        // Message ID
        ack[1] = messageId & 0xFF;
        ack[2] = (messageId >> 8) & 0xFF;
        ack[3] = (messageId >> 16) & 0xFF;
        ack[4] = (messageId >> 24) & 0xFF;

        // Chunk index
        ack[5] = chunkIndex & 0xFF;
        ack[6] = (chunkIndex >> 8) & 0xFF;
        ack[7] = (chunkIndex >> 16) & 0xFF;
        ack[8] = (chunkIndex >> 24) & 0xFF;

        return ack;
    }

    class ChunkedMessageHandler::Impl {
    public:
        struct OutgoingMessage {
            std::vector<std::vector<uint8_t>> chunks;
            std::set<uint32_t> acknowledgedChunks;
            std::chrono::steady_clock::time_point lastActivity;
        };

        std::map<uint32_t, std::vector<std::vector<uint8_t>>> incomingMessages;
        std::map<uint32_t, OutgoingMessage> outgoingMessages;
        AckReceivedCallback ackCallback;
    };

    ChunkedMessageHandler::ChunkedMessageHandler()
        : m_impl(std::make_unique<Impl>()) {
    }

    ChunkedMessageHandler::~ChunkedMessageHandler() = default;

    uint32_t ChunkedMessageHandler::SendMessage(const std::vector<uint8_t>& message, ChunkSendCallback sendCallback) {
        auto chunks = BleMessageChunker::CreateChunks(message);
        if (chunks.empty()) {
            return 0;
        }

        const uint32_t messageId = BleMessageChunker::GetMessageId(chunks.front());
        Impl::OutgoingMessage outgoing;
        outgoing.chunks = chunks;
        outgoing.lastActivity = std::chrono::steady_clock::now();

        for (const auto& chunk : outgoing.chunks) {
            if (sendCallback) {
                sendCallback(chunk);
            }
        }

        m_impl->outgoingMessages[messageId] = std::move(outgoing);
        return messageId;
    }

    void ChunkedMessageHandler::ProcessReceivedData(const std::vector<uint8_t>& data, MessageCompleteCallback onComplete) {
        if (!BleMessageChunker::IsValidChunk(data)) {
            return;
        }

        const uint32_t messageId = BleMessageChunker::GetMessageId(data);
        const uint32_t chunkIndex = BleMessageChunker::GetChunkIndex(data);

        if (BleMessageChunker::IsAck(data)) {
            auto outgoingIt = m_impl->outgoingMessages.find(messageId);
            if (outgoingIt != m_impl->outgoingMessages.end()) {
                outgoingIt->second.acknowledgedChunks.insert(chunkIndex);
                outgoingIt->second.lastActivity = std::chrono::steady_clock::now();
                if (m_impl->ackCallback) {
                    m_impl->ackCallback(messageId, chunkIndex);
                }
                if (outgoingIt->second.acknowledgedChunks.size() >= outgoingIt->second.chunks.size()) {
                    m_impl->outgoingMessages.erase(outgoingIt);
                }
            }
            return;
        }

        std::vector<uint8_t> completeMessage;
        if (BleMessageChunker::ProcessChunk(data, m_impl->incomingMessages, completeMessage) && onComplete) {
            onComplete(messageId, completeMessage);
        }
    }

    void ChunkedMessageHandler::SetAckCallback(AckReceivedCallback callback) {
        m_impl->ackCallback = callback;
    }

    void ChunkedMessageHandler::GetStatistics(size_t& outgoing, size_t& incoming) const {
        outgoing = m_impl->outgoingMessages.size();
        incoming = m_impl->incomingMessages.size();
    }

    size_t ChunkedMessageHandler::CleanupTimedOut(int64_t timeoutMs) {
        const auto now = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::milliseconds(timeoutMs);
        size_t removed = 0;

        for (auto it = m_impl->outgoingMessages.begin(); it != m_impl->outgoingMessages.end();) {
            if (now - it->second.lastActivity > timeout) {
                it = m_impl->outgoingMessages.erase(it);
                ++removed;
            }
            else {
                ++it;
            }
        }

        return removed;
    }

} // namespace BleServer
