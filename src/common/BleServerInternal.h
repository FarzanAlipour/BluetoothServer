// BleServerInternal.h - C++-only introspection hooks for tests. Not part of the public ABI.
#pragma once
#include <cstddef>

namespace BleServer {
namespace Internal {

    // Number of partially received chunks currently buffered by a chunker handle (0 for invalid handles).
    size_t ChunkerBufferedChunks(void* chunker);
    // Number of sent messages still waiting for ACKs (0 for invalid handles).
    size_t ChunkerOutstandingMessages(void* chunker);
    // Number of live server / chunker handles (leak check).
    size_t LiveServerHandles();
    size_t LiveChunkerHandles();

    // Bounds enforced by the chunker wrapper.
    static constexpr size_t kMaxPartialMessages = 8;       // incoming messages being reassembled
    static constexpr size_t kMaxOutstandingMessages = 8;   // sent messages awaiting ACKs
    static constexpr int kMaxChunkRetries = 5;             // retransmissions per chunk before failing

} // namespace Internal
} // namespace BleServer
