// BleMessageChunker.h - Message chunking interface for BLE
#pragma once

#include <vector>
#include <map>
#include <cstdint>
#include <memory>
#include <functional>
#include <set>
#include <chrono>
#include <atomic>

namespace BleServer {

    /**
     * BleMessageChunker - Handles splitting and reassembling messages for BLE transport
     *
     * BLE has limited packet sizes (typically 20-512 bytes depending on MTU).
     * This class provides a protocol for splitting large messages into chunks
     * and reassembling them on the receiving side.
     *
     * Protocol Format:
     * - Each chunk is 256 bytes maximum
     * - Header is 13 bytes
     * - Payload is up to 243 bytes
     *
     * Header Structure:
     * - Byte 0: Message type (0x01 = data, 0x02 = ack)
     * - Bytes 1-4: Message ID (32-bit, little-endian)
     * - Bytes 5-8: Chunk index (32-bit, little-endian)
     * - Bytes 9-12: Total chunks (32-bit, little-endian)
     */
    class BleMessageChunker {
    public:
        // Constants
        static constexpr size_t CHUNK_SIZE = 256;        // Total chunk size including header
        static constexpr size_t HEADER_SIZE = 13;        // Header size in bytes
        static constexpr size_t MAX_PAYLOAD_SIZE = CHUNK_SIZE - HEADER_SIZE;  // 243 bytes
        static constexpr uint32_t MAX_CHUNKS_PER_MESSAGE = 1000;
        static constexpr size_t MAX_MESSAGE_SIZE = MAX_PAYLOAD_SIZE * MAX_CHUNKS_PER_MESSAGE;

        // BLE MTU / MRU
        static constexpr size_t BLE_MTU = 23;           // Default BLE packet MTU
        static constexpr size_t BLE_MRU = 517;          // Maximum BLE payload with L2CAP

        // Message types
        enum MessageType : uint8_t {
            DATA_CHUNK = 0x01,
            ACK_CHUNK = 0x02
        };

        /**
         * Creates chunks from a message
         * @param message The complete message to split
         * @return Vector of chunks ready to send over BLE
         */
        static std::vector<std::vector<uint8_t>> CreateChunks(const std::vector<uint8_t>& message) {
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
                chunk.push_back(DATA_CHUNK); // Data chunk type

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
                    message.begin() + static_cast<std::ptrdiff_t>(offset),
                    message.begin() + static_cast<std::ptrdiff_t>(offset + payloadSize));

                chunks.push_back(std::move(chunk));
            }

            return chunks;
        }

        /**
         * Process a received chunk and check if message is complete
         * @param chunk The received chunk data
         * @param incomingMessages Map storing incomplete messages (managed by caller)
         * @param completeMessage Output parameter filled when message is complete
         * @return true if message is complete, false if waiting for more chunks
         */
        static bool ProcessChunk(const std::vector<uint8_t>& chunk,
            std::map<uint32_t, std::vector<std::vector<uint8_t>>>& incomingMessages,
            std::vector<uint8_t>& completeMessage) {
            if (!IsValidChunk(chunk) || IsAck(chunk)) {
                return false;
            }

            // Parse header
            uint8_t type = chunk[0];

            if (type != DATA_CHUNK) {
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
            messageChunks[chunkIndex] = std::vector<uint8_t>(chunk.begin() + static_cast<std::ptrdiff_t>(HEADER_SIZE), chunk.end());

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

        /**
         * Create an acknowledgment packet
         * @param messageId The message ID to acknowledge
         * @param chunkIndex The chunk index being acknowledged
         * @return ACK packet ready to send
         */
        static std::vector<uint8_t> CreateAck(uint32_t messageId, uint32_t chunkIndex) {
            std::vector<uint8_t> ack(HEADER_SIZE, 0);

            ack[0] = ACK_CHUNK; // ACK type

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

        /**
         * Check whether a packet is an acknowledgment packet.
         */
        static bool IsAck(const std::vector<uint8_t>& chunk) {
            return chunk.size() >= HEADER_SIZE && chunk[0] == ACK_CHUNK;
        }

        /**
         * Generate a unique message ID
         * @return Unique 32-bit message ID
         */
        static uint32_t GenerateMessageId() {
            static std::atomic<uint32_t> g_messageIdCounter{0};
            return ++g_messageIdCounter;
        }

        // Utility functions for working with chunks

        /**
         * Extract message ID from a chunk
         * @param chunk The chunk data
         * @return Message ID or 0 if invalid
         */
        static uint32_t GetMessageId(const std::vector<uint8_t>& chunk) {
            if (chunk.size() < HEADER_SIZE) return 0;
            return chunk[1] | (chunk[2] << 8) | (chunk[3] << 16) | (chunk[4] << 24);
        }

        /**
         * Extract chunk index from a chunk
         * @param chunk The chunk data
         * @return Chunk index or 0 if invalid
         */
        static uint32_t GetChunkIndex(const std::vector<uint8_t>& chunk) {
            if (chunk.size() < HEADER_SIZE) return 0;
            return chunk[5] | (chunk[6] << 8) | (chunk[7] << 16) | (chunk[8] << 24);
        }

        /**
         * Extract total chunks from a chunk
         * @param chunk The chunk data
         * @return Total chunks or 0 if invalid
         */
        static uint32_t GetTotalChunks(const std::vector<uint8_t>& chunk) {
            if (chunk.size() < HEADER_SIZE) return 0;
            return chunk[9] | (chunk[10] << 8) | (chunk[11] << 16) | (chunk[12] << 24);
        }

        /**
         * Check if a chunk is valid
         * @param chunk The chunk data
         * @return true if chunk has valid structure
         */
        static bool IsValidChunk(const std::vector<uint8_t>& chunk) {
            if (chunk.size() < HEADER_SIZE) return false;
            uint8_t type = chunk[0];
            if (type != DATA_CHUNK && type != ACK_CHUNK) return false;
            uint32_t chunkIndex = GetChunkIndex(chunk);
            uint32_t totalChunks = GetTotalChunks(chunk);
            if (type == ACK_CHUNK) {
                return totalChunks == 0;
            }
            return chunkIndex < totalChunks && totalChunks > 0 && totalChunks <= MAX_CHUNKS_PER_MESSAGE;
        }
    };

    /**
     * ChunkedMessageHandler - Higher-level handler for chunked messages
     * Manages the complete lifecycle of sending and receiving chunked messages
     */
    class ChunkedMessageHandler {
    public:
        // Callback types
        using MessageCompleteCallback = std::function<void(uint32_t messageId, const std::vector<uint8_t>& message)>;
        using ChunkSendCallback = std::function<void(const std::vector<uint8_t>& chunk)>;
        using AckReceivedCallback = std::function<void(uint32_t messageId, uint32_t chunkIndex)>;

        ChunkedMessageHandler();
        ~ChunkedMessageHandler();

        /**
         * Send a message with automatic chunking
         * @param message The message to send
         * @param sendCallback Callback to send each chunk
         * @return Message ID for tracking
         */
        uint32_t SendMessage(const std::vector<uint8_t>& message, ChunkSendCallback sendCallback);

        /**
         * Process received data (could be chunk or ACK)
         * @param data The received data
         * @param onComplete Callback when a complete message is received
         */
        void ProcessReceivedData(const std::vector<uint8_t>& data, MessageCompleteCallback onComplete);

        /**
         * Set callback for when ACKs are received
         * @param callback The callback function
         */
        void SetAckCallback(AckReceivedCallback callback);

        /**
         * Get statistics about pending messages
         * @param outgoing Number of outgoing messages waiting for ACKs
         * @param incoming Number of incomplete incoming messages
         */
        void GetStatistics(size_t& outgoing, size_t& incoming) const;

        /**
         * Clean up timed-out messages
         * @param timeoutMs Timeout in milliseconds (default 30 seconds)
         * @return Number of messages cleaned up
         */
        size_t CleanupTimedOut(int64_t timeoutMs = 30000);

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };

    // Debugging utilities (only available in debug builds)
#ifdef BLE_CHUNKER_DEBUG
    void PrintChunk(const std::vector<uint8_t>& chunk);
    bool TestChunking();
#endif

} // namespace BleServer
