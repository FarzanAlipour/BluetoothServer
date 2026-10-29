package com.bluetoothserver.blelink.session

/**
 * Receive-side limits this side advertises in its HELLO (PROTOCOL.md section 4.1).
 *
 * @param maxPacket largest packet we can receive; null = derive from the OS limit (ATT_MTU - 3).
 * @param maxMessage largest message we accept; 0xFFFFFFFF = no limit. Bounds receive allocations.
 * @param window number of messages we accept concurrently (>= 1).
 * @param wantProgress flags bit 0: ask the peer for PROGRESS packets (and offer to send them).
 * @param timeout idle timeout we need, in 0.25 s units, 0 = default 40 (10 s).
 */
data class LocalLimits(
    val maxPacket: Int? = null,
    val maxMessage: Long = 16L * 1024 * 1024,
    val window: Int = 4,
    val wantProgress: Boolean = true,
    val timeout: Int = 0,
) {
    init {
        require(window in 1..255) { "window must be 1..255" }
        require(maxMessage in 0..0xFFFFFFFFL) { "maxMessage must fit in u32" }
        require(timeout in 0..255) { "timeout must fit in u8" }
    }
}

/** What the peer advertised in its HELLO, normalised (maxPacket clamped to 20..512, window >= 1). */
data class PeerLimits(
    val version: Int,
    val maxPacket: Int,
    val maxMessage: Long,
    val window: Int,
    val flags: Int,
    val timeout: Int,
) {
    val wantsProgress: Boolean get() = flags and 0x01 != 0
    val streamingReceiver: Boolean get() = flags and 0x02 != 0
    val unlimitedMessage: Boolean get() = maxMessage == 0xFFFFFFFFL
}

/** Why a sent message failed (PROTOCOL.md section 18 names plus local `cancelled`). */
enum class FailReason { DISCONNECTED, RESET, TIMEOUT, CRC, TOO_LARGE, BUSY, REJECTED, LINK_UNRELIABLE, CANCELLED }

enum class SessionState { CLOSED, OPENING, READY }

sealed class SessionEvent {
    /** Both HELLOs exchanged. `reset` = the peer re-sent HELLO while READY (in-flight messages failed with RESET). */
    data class PeerReady(val peer: PeerLimits, val sendPacketSize: Int, val reset: Boolean) : SessionEvent()

    /** A complete, CRC-verified message from the peer (already ACKed with status 0). */
    class Received(val msgId: Int, val data: ByteArray) : SessionEvent()

    /** The peer ACKed our message with status 0. */
    data class Sent(val msgId: Int) : SessionEvent()

    /** Our message failed. `ackStatus` is the peer's ACK status when the failure came from an ACK. */
    data class Failed(val msgId: Int, val reason: FailReason, val ackStatus: Int? = null) : SessionEvent()

    /** Bytes of our message handed to the stack (throttled to ~1 % steps). */
    data class SendProgress(val msgId: Int, val bytesSent: Long, val total: Long) : SessionEvent()

    /** The peer's PROGRESS packet for our message (advisory). */
    data class PeerProgress(val msgId: Int, val bytesReceived: Long, val total: Long) : SessionEvent()

    /** Bytes received of an incoming message (throttled to ~1 % steps; data is provisional until Received). */
    data class ReceiveProgress(val msgId: Int, val bytesReceived: Long, val total: Long) : SessionEvent()

    /** No server HELLO after the initial HELLO and 3 resends: the transport should disconnect. */
    object HandshakeFailed : SessionEvent()
}

/** One packet to write to RX. HELLO is written with response (PROTOCOL.md section 1), data without. */
class Outgoing(val bytes: ByteArray, val withResponse: Boolean)
