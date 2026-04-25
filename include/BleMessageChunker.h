// BleMessageChunker.h - Message chunking interface for BLE
#pragma once

#include <vector>
#include <map>
#include <cstdint>
#include <memory>
#include <functional>
#include <set>
#include <chrono>

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
        static std::vector<std::vector<uint8_t>> CreateChunks(const std::vector<uint8_t>& message);

        /**
         * Process a received chunk and check if message is complete
         * @param chunk The received chunk data
         * @param incomingMessages Map storing incomplete messages (managed by caller)
         * @param completeMessage Output parameter filled when message is complete
         * @return true if message is complete, false if waiting for more chunks
         */
        static bool ProcessChunk(const std::vector<uint8_t>& chunk,
            std::map<uint32_t, std::vector<std::vector<uint8_t>>>& incomingMessages,
            std::vector<uint8_t>& completeMessage);

        /**
         * Create an acknowledgment packet
         * @param messageId The message ID to acknowledge
         * @param chunkIndex The chunk index being acknowledged
         * @return ACK packet ready to send
         */
        static std::vector<uint8_t> CreateAck(uint32_t messageId, uint32_t chunkIndex);

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
        static uint32_t GenerateMessageId();

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