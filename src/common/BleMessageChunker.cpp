// BleMessageChunker.cpp - ChunkedMessageHandler & debug implementations
#include "BleMessageChunker.h"
#include <algorithm>
#include <chrono>

namespace BleServer {

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
