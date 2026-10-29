package com.bluetoothserver.blelink

import com.bluetoothserver.blelink.session.BleLinkSession
import com.bluetoothserver.blelink.session.FailReason
import com.bluetoothserver.blelink.session.LocalLimits
import com.bluetoothserver.blelink.session.PeerLimits
import com.bluetoothserver.blelink.session.SessionEvent
import com.bluetoothserver.blelink.session.SessionState
import com.bluetoothserver.blelink.transport.PacketTransport
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/** Connection state of a [BleLinkClient]. */
enum class LinkState { DISCONNECTED, CONNECTING, OPENING, READY }

/** Events of a [BleLinkClient] (all directions of one connection). */
sealed class BleLinkEvent {
    /** HELLO exchanged. `reset` = the server re-sent HELLO (our in-flight messages failed with RESET). */
    data class PeerReady(val peer: PeerLimits, val sendPacketSize: Int, val reset: Boolean) : BleLinkEvent()

    /** A complete, CRC-verified message from the server. */
    class Received(val msgId: Int, val data: ByteArray) : BleLinkEvent()

    /** The server ACKed our message [msgId] with status OK. */
    data class Sent(val msgId: Int) : BleLinkEvent()

    /** Our message failed. `ackStatus` is the server's ACK status when the failure came from an ACK. */
    data class Failed(val msgId: Int, val reason: FailReason, val ackStatus: Int? = null) : BleLinkEvent()

    /** Progress of one message; see [Kind]. Throttled to about 1 % steps (PEER_CONFIRMED: as the peer sends it). */
    data class Progress(val msgId: Int, val kind: Kind, val bytes: Long, val total: Long) : BleLinkEvent() {
        enum class Kind {
            /** Our bytes handed to the Bluetooth stack. */
            SENT_TO_STACK,
            /** The server's PROGRESS packet (advisory) for our message. */
            PEER_CONFIRMED,
            /** Bytes of an incoming message (provisional until [Received]). */
            RECEIVING,
        }
    }

    /** The link is gone. All unfinished sends were reported as [Failed] with DISCONNECTED before this. */
    data class Disconnected(val reason: String, val status: Int?) : BleLinkEvent()
}

/**
 * BleLink v1 client (central) on top of a [PacketTransport] (normally
 * [com.bluetoothserver.blelink.transport.GattTransport]).
 *
 * Thread-safe: every method may be called from any thread. Events are published on [events]
 * (unbounded buffer; subscribe before [connect] so you do not miss PeerReady).
 */
class BleLinkClient(
    private val transport: PacketTransport,
    limits: LocalLimits = LocalLimits(),
    private val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Default),
    private val tickMs: Long = 250,
    clock: () -> Long = System::currentTimeMillis,
) {
    private val lock = Any()
    private val _events = MutableSharedFlow<BleLinkEvent>(extraBufferCapacity = Int.MAX_VALUE)
    private val _state = MutableStateFlow(LinkState.DISCONNECTED)

    val events: SharedFlow<BleLinkEvent> = _events.asSharedFlow()
    val state: StateFlow<LinkState> = _state.asStateFlow()

    private val session = BleLinkSession(limits, clock, ::onSessionEvent)
    private var writing = false
    private var highThroughput = false
    private var ticker: Job? = null

    /** Server limits once READY, else null. */
    val peer: PeerLimits? get() = synchronized(lock) { session.peer }

    /** Payload bytes per packet we send (header included), 20 until READY. */
    val sendPacketSize: Int get() = synchronized(lock) { session.sendPacketSize }

    /** Connects the transport; HELLO is sent as soon as TX notifications are enabled. */
    fun connect() {
        synchronized(lock) {
            if (_state.value != LinkState.DISCONNECTED) return
            _state.value = LinkState.CONNECTING
            writing = false
            highThroughput = false
        }
        transport.start(transportListener)
    }

    /** Suspends until READY; returns the server limits. Throws [IllegalStateException] on disconnect. */
    suspend fun awaitReady(): PeerLimits {
        val s = state.first { it == LinkState.READY || it == LinkState.DISCONNECTED }
        check(s == LinkState.READY) { "disconnected before the session was ready" }
        return peer ?: error("disconnected before the session was ready")
    }

    /**
     * Queues a message (any size up to the server's maxMessage) and returns its msgId. Allowed while
     * OPENING (it waits for READY). Outcome: [BleLinkEvent.Sent] or [BleLinkEvent.Failed].
     */
    fun send(data: ByteArray): Int = synchronized(lock) {
        check(_state.value == LinkState.OPENING || _state.value == LinkState.READY) { "not connected (${_state.value})" }
        val id = session.send(data)
        pump()
        id
    }

    /** Cancels a queued or in-flight message (CANCEL is sent if it was started). */
    fun cancel(msgId: Int): Boolean = synchronized(lock) { session.cancel(msgId).also { pump() } }

    /** Re-sends HELLO (session reset): e.g. after the app restarted while the link stayed up. */
    fun resetSession() = synchronized(lock) {
        if (_state.value == LinkState.OPENING || _state.value == LinkState.READY) {
            session.open(session.osLimit)
            _state.value = LinkState.OPENING
            pump()
        }
    }

    /** Disconnects. Unfinished sends fail with DISCONNECTED, then [BleLinkEvent.Disconnected] follows. */
    fun disconnect() = transport.close()

    // ------------------------------------------------------------------ internals

    private val transportListener = object : PacketTransport.Listener {
        override fun onLinkReady(osLimit: Int) = synchronized(lock) {
            session.open(osLimit)
            _state.value = LinkState.OPENING
            startTicker()
            pump()
        }

        override fun onOsLimitChanged(osLimit: Int) = synchronized(lock) { session.updateOsLimit(osLimit) }

        override fun onPacket(bytes: ByteArray) = synchronized(lock) {
            session.onPacket(bytes)
            pump()
        }

        override fun onWriteComplete(success: Boolean) = synchronized(lock) {
            // A refused write is a lost packet: the peer sees the seq gap and RESUMEs (PROTOCOL.md 8).
            writing = false
            session.onPacketWritten()
            pump()
        }

        override fun onDisconnected(status: Int?, reason: String) {
            synchronized(lock) {
                ticker?.cancel(); ticker = null
                writing = false
                session.onDisconnected()
                _state.value = LinkState.DISCONNECTED
            }
            _events.tryEmit(BleLinkEvent.Disconnected(reason, status))
        }
    }

    private fun startTicker() {
        ticker?.cancel()
        ticker = scope.launch {
            while (isActive) {
                delay(tickMs)
                synchronized(lock) {
                    if (_state.value == LinkState.OPENING || _state.value == LinkState.READY) {
                        session.onTick()
                        pump()
                    }
                }
            }
        }
    }

    /** Writes the next packet if none is outstanding (one write at a time, PROTOCOL.md 6.2). Holds [lock]. */
    private fun pump() {
        val active = session.activeMessages > 0
        if (active != highThroughput) { highThroughput = active; transport.setHighThroughput(active) }
        if (writing) return
        val out = session.poll() ?: return
        writing = true
        transport.write(out.bytes, out.withResponse)
    }

    private fun onSessionEvent(e: SessionEvent) {
        val mapped = when (e) {
            is SessionEvent.PeerReady -> { _state.value = LinkState.READY; BleLinkEvent.PeerReady(e.peer, e.sendPacketSize, e.reset) }
            is SessionEvent.Received -> BleLinkEvent.Received(e.msgId, e.data)
            is SessionEvent.Sent -> BleLinkEvent.Sent(e.msgId)
            is SessionEvent.Failed -> BleLinkEvent.Failed(e.msgId, e.reason, e.ackStatus)
            is SessionEvent.SendProgress -> BleLinkEvent.Progress(e.msgId, BleLinkEvent.Progress.Kind.SENT_TO_STACK, e.bytesSent, e.total)
            is SessionEvent.PeerProgress -> BleLinkEvent.Progress(e.msgId, BleLinkEvent.Progress.Kind.PEER_CONFIRMED, e.bytesReceived, e.total)
            is SessionEvent.ReceiveProgress -> BleLinkEvent.Progress(e.msgId, BleLinkEvent.Progress.Kind.RECEIVING, e.bytesReceived, e.total)
            SessionEvent.HandshakeFailed -> {
                // no server HELLO after 3 resends (PROTOCOL.md 10): drop the link
                scope.launch { transport.close() }
                null
            }
        }
        if (mapped != null) _events.tryEmit(mapped)
    }
}
