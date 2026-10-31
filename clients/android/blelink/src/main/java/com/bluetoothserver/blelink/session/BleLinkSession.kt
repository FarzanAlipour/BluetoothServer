package com.bluetoothserver.blelink.session

import com.bluetoothserver.blelink.codec.AckStatus
import com.bluetoothserver.blelink.codec.CancelReason
import com.bluetoothserver.blelink.codec.Crc32
import com.bluetoothserver.blelink.codec.Packet
import com.bluetoothserver.blelink.codec.PacketCodec
import com.bluetoothserver.blelink.codec.PacketType

/**
 * Client (central) side of a BleLink v1 session (docs/PROTOCOL.md), without any Bluetooth code.
 *
 * Not thread-safe: the owner calls every method from one thread (the Android transport uses one
 * HandlerThread). Events are delivered synchronously through [listener].
 *
 * Driving it:
 *  - [open] once TX notifications are enabled (queues HELLO),
 *  - [poll] whenever the transport may write one packet (flow control: one outstanding write),
 *    then [onPacketWritten] when the stack accepted it,
 *  - [onPacket] for every TX notification, [onTick] periodically (every 250 ms is fine),
 *  - [onDisconnected] when the link drops.
 */
class BleLinkSession(
    private val limits: LocalLimits = LocalLimits(),
    private val clock: () -> Long = System::currentTimeMillis,
    private val listener: (SessionEvent) -> Unit,
) {
    var state: SessionState = SessionState.CLOSED
        private set
    var peer: PeerLimits? = null
        private set

    /** OS limit for one write (Android: negotiated MTU - 3). */
    var osLimit: Int = MIN_PACKET
        private set

    // --- seq / handshake ---
    private var txSeq = 0
    private var expectedSeq = 0
    private var pendingHelloReplies = 0
    private var helloAttempts = 0
    private var helloSentAt = 0L
    private var helloQueued = false
    private var answerHelloQueued = false

    // --- send side ---
    internal var nextMsgId = 0
    private val queued = ArrayDeque<OutMsg>()
    private val inFlight = LinkedHashMap<Int, OutMsg>()
    private var lastServedId: Int? = null

    // --- receive side ---
    private val incoming = LinkedHashMap<Int, InMsg>()
    private val completed = ArrayDeque<IntArray>() // [msgId, status], last `window` completed
    private val rejected = ArrayDeque<IntArray>()  // [msgId, status] early ACKs (2, 3)
    private val unknownResumed = HashMap<Int, Long>()

    // control packets (HELLO, ACK, RESUME, CANCEL, PROGRESS) go before data; seq is assigned at poll time
    private val control = ArrayDeque<(Int) -> Packet>()

    /** Effective packet size for packets we send (PROTOCOL.md section 5). */
    val sendPacketSize: Int
        get() {
            val p = peer ?: return MIN_PACKET
            if (state != SessionState.READY) return MIN_PACKET
            return minOf(p.maxPacket, osLimit).coerceIn(MIN_PACKET, MAX_PACKET)
        }

    /** Idle/ACK/HELLO timeout T in ms: max of both sides' HELLO `timeout` (0 -> 40), 0.25 s units. */
    val timeoutMs: Long
        get() {
            val mine = if (limits.timeout == 0) 40 else limits.timeout
            val theirs = peer?.timeout?.let { if (it == 0) 40 else it } ?: 0
            return maxOf(mine, theirs) * 250L
        }

    /** Starts (or restarts) the session: queues our HELLO. Call after the TX CCCD write completed. */
    fun open(osLimit: Int) {
        this.osLimit = osLimit.coerceIn(MIN_PACKET, MAX_PACKET)
        if (state == SessionState.READY) resetLocal(FailReason.RESET)
        state = SessionState.OPENING
        helloAttempts = 0
        control.clear()
        helloQueued = false
        answerHelloQueued = false
        queueHello()
    }

    /** The OS write limit changed. Only affects packets we send; re-[open] to advertise a new maxPacket. */
    fun updateOsLimit(osLimit: Int) {
        this.osLimit = osLimit.coerceIn(MIN_PACKET, MAX_PACKET)
    }

    /** Queues a message. Returns its msgId. Messages wait until READY and the peer's window allows. */
    fun send(data: ByteArray): Int {
        check(state != SessionState.CLOSED) { "session is closed" }
        require(data.size.toLong() <= 0xFFFFFFFFL)
        val id = nextMsgId
        nextMsgId = (nextMsgId + 1) and 0xFFFF
        queued.addLast(OutMsg(id, data, Crc32.of(data)))
        return id
    }

    /** Cancels a queued or in-flight message (sends CANCEL reason 0 if it was started). */
    fun cancel(msgId: Int): Boolean {
        queued.firstOrNull { it.id == msgId }?.let {
            queued.remove(it); emit(SessionEvent.Failed(msgId, FailReason.CANCELLED)); return true
        }
        inFlight.remove(msgId)?.let {
            control.addLast { s -> Packet.Cancel(s, msgId, CancelReason.APPLICATION) }
            emit(SessionEvent.Failed(msgId, FailReason.CANCELLED)); return true
        }
        return false
    }

    /** Messages queued, in flight or being received (used as the "transfer active" throughput hint). */
    val activeMessages: Int get() = queued.size + inFlight.size + incoming.size

    fun hasPendingWrites(): Boolean =
        control.isNotEmpty() || (state == SessionState.READY &&
            (inFlight.values.any { it.sending } || (queued.isNotEmpty() && inFlight.size < (peer?.window ?: 1))))

    /** Returns the next packet to write now, or null. The caller MUST write it before polling again. */
    fun poll(): Outgoing? {
        control.removeFirstOrNull()?.let { make ->
            val seq = txSeq
            val p = make(seq)
            if (p is Packet.Hello) {
                txSeq = 1
                if (answerHelloQueued) {
                    answerHelloQueued = false // an answer to the peer's HELLO: the peer does not answer it
                } else {
                    helloQueued = false
                    pendingHelloReplies++
                    helloSentAt = clock()
                }
                return Outgoing(PacketCodec.encode(p), withResponse = true)
            }
            txSeq = (txSeq + 1) and 0xFF
            return Outgoing(PacketCodec.encode(p), withResponse = false)
        }
        if (state != SessionState.READY) return null
        promoteQueued()
        val m = nextSending() ?: return null
        val seq = txSeq
        txSeq = (txSeq + 1) and 0xFF
        return Outgoing(PacketCodec.encode(buildDataPacket(m, seq)), withResponse = false)
    }

    /** The stack accepted the last polled packet (Android: onCharacteristicWrite). Restarts ACK timers. */
    fun onPacketWritten() {
        val now = clock()
        for (m in inFlight.values) if (!m.sending && m.waitingSinceWrite) {
            m.lastActivity = now; m.waitingSinceWrite = false
        }
    }

    fun onPacket(bytes: ByteArray) {
        if (state == SessionState.CLOSED) return
        val p = PacketCodec.decode(bytes) ?: return
        if (p is Packet.Hello) { onHello(p); return }
        if (state != SessionState.READY) return // section 3: ignore before READY
        if (p.seq != expectedSeq) onSeqGap()
        expectedSeq = (p.seq + 1) and 0xFF
        when (p) {
            is Packet.MsgStart -> onMsgStart(p)
            is Packet.MsgData -> onMsgData(p.msgId, p.payload)
            is Packet.MsgResend -> onMsgResend(p)
            is Packet.Ack -> onAck(p)
            is Packet.Resume -> onResume(p)
            is Packet.Cancel -> { incoming.remove(p.msgId); unknownResumed.remove(p.msgId) }
            is Packet.Progress -> inFlight[p.msgId]?.let {
                emit(SessionEvent.PeerProgress(p.msgId, p.bytesReceived, it.data.size.toLong()))
            }
            else -> Unit // unknown type: ignored, seq already counted
        }
    }

    fun onTick() {
        if (state == SessionState.CLOSED) return
        val now = clock()
        val t = timeoutMs
        if (state == SessionState.OPENING && pendingHelloReplies > 0 && !helloQueued && now - helloSentAt >= t) {
            if (helloAttempts >= HELLO_RESENDS) {
                onDisconnected(); emit(SessionEvent.HandshakeFailed); return
            }
            helloAttempts++
            queueHello()
        }
        if (state != SessionState.READY) return
        // sender: ACK timeout -> resend whole message with the same msgId (max 3)
        for (m in inFlight.values.toList()) {
            if (m.sending || m.waitingSinceWrite || now - m.lastActivity < t) continue
            if (m.ackResends >= ACK_RESENDS) {
                failInFlight(m, FailReason.TIMEOUT); sendCancel(m.id, CancelReason.GAVE_UP)
            } else {
                m.ackResends++; m.restart(0); m.restartOffset = null
            }
        }
        // receiver: idle timeout -> RESUME(received); give up after 3 RESUMEs without progress
        for (m in incoming.values.toList()) {
            if (now - m.lastActivity < t) continue
            if (m.idleResumes >= IDLE_RESUMES) { incoming.remove(m.id); continue }
            m.idleResumes++
            m.resync = true
            m.lastActivity = now
            queueResume(m.id, m.received)
        }
        unknownResumed.entries.removeAll { now - it.value >= t }
    }

    fun onDisconnected() {
        if (state == SessionState.CLOSED) return
        state = SessionState.CLOSED
        val failed = inFlight.keys.toList() + queued.map { it.id }
        inFlight.clear(); queued.clear()
        clearReceive()
        control.clear()
        pendingHelloReplies = 0
        helloQueued = false
        answerHelloQueued = false
        peer = null
        failed.forEach { emit(SessionEvent.Failed(it, FailReason.DISCONNECTED)) }
    }

    // ------------------------------------------------------------------ handshake

    private fun queueHello() {
        if (helloQueued) return
        helloQueued = true
        control.addFirst { _ -> helloPacket() }
    }

    private fun helloPacket(): Packet.Hello {
        val mp = (limits.maxPacket ?: osLimit).coerceIn(MIN_PACKET, MAX_PACKET)
        val flags = if (limits.wantProgress) PacketType.FLAG_PROGRESS else 0
        return Packet.Hello(0, 1, mp, limits.maxMessage, limits.window, flags, limits.timeout)
    }

    private fun onHello(h: Packet.Hello) {
        val p = PeerLimits(
            version = h.version,
            maxPacket = h.maxPacket.coerceIn(MIN_PACKET, MAX_PACKET),
            maxMessage = h.maxMessage,
            window = maxOf(1, h.window),
            flags = h.flags,
            timeout = h.timeout,
        )
        peer = p
        expectedSeq = 1
        if (pendingHelloReplies > 0) {
            // answer to a HELLO we sent: the peer reset its state for us, so drop what we were receiving
            pendingHelloReplies--
            clearReceive()
            val wasReady = state == SessionState.READY
            state = SessionState.READY
            if (!wasReady) emit(SessionEvent.PeerReady(p, sendPacketSize, reset = false))
            return
        }
        if (helloQueued) {
            // our HELLO is queued but not yet written: this HELLO is not an answer to it; the queued one
            // will (re)start the session anyway, so just wait for its answer.
            clearReceive()
            return
        }
        // peer-initiated reset (section 3): fail in-flight, discard partials, answer with our HELLO
        resetLocal(FailReason.RESET)
        control.clear()
        control.addFirst { _ -> helloPacket() }
        answerHelloQueued = true // not counted as a pending reply: the peer does not answer an answer
        state = SessionState.READY
        emit(SessionEvent.PeerReady(p, sendPacketSize, reset = true))
    }

    private fun resetLocal(reason: FailReason) {
        val failed = inFlight.keys.toList()
        inFlight.clear()
        lastServedId = null
        clearReceive()
        failed.forEach { emit(SessionEvent.Failed(it, reason)) }
    }

    private fun clearReceive() {
        incoming.clear(); completed.clear(); rejected.clear(); unknownResumed.clear()
    }

    // ------------------------------------------------------------------ sender

    private fun promoteQueued() {
        val p = peer ?: return
        while (queued.isNotEmpty() && inFlight.size < p.window) {
            val m = queued.removeFirst()
            if (!p.unlimitedMessage && m.data.size.toLong() > p.maxMessage) {
                emit(SessionEvent.Failed(m.id, FailReason.TOO_LARGE)); continue
            }
            m.restart(0)
            inFlight[m.id] = m
        }
    }

    /** Round-robin over in-flight messages that still have packets to send. */
    private fun nextSending(): OutMsg? {
        val all = inFlight.values.toList()
        if (all.isEmpty()) return null
        val start = all.indexOfFirst { it.id == lastServedId } // -1 when gone: start from the oldest
        for (k in 1..all.size) {
            val m = all[(start + k).mod(all.size)]
            if (m.sending) { lastServedId = m.id; return m }
        }
        return null
    }

    private fun buildDataPacket(m: OutMsg, seq: Int): Packet {
        val ps = sendPacketSize
        val total = m.data.size
        val p: Packet = when {
            m.startPending -> {
                val n = minOf(PacketCodec.startCapacity(ps), total)
                m.startPending = false
                m.offset = n
                Packet.MsgStart(seq, m.id, total.toLong(), m.crc, m.data.copyOfRange(0, n))
            }
            m.resendPending -> {
                val from = m.offset
                val n = minOf(PacketCodec.resendCapacity(ps), total - from)
                m.resendPending = false
                m.offset = from + n
                Packet.MsgResend(seq, m.id, from.toLong(), m.data.copyOfRange(from, from + n))
            }
            else -> {
                val from = m.offset
                val n = minOf(PacketCodec.dataCapacity(ps), total - from)
                m.offset = from + n
                Packet.MsgData(seq, m.id, m.data.copyOfRange(from, from + n))
            }
        }
        m.restartOffset = null // "until the next packet it sends after restart" (section 8.2)
        m.lastActivity = clock()
        if (m.offset >= total) { m.sending = false; m.waitingSinceWrite = true }
        if (total > 0) {
            val pct = (m.offset.toLong() * 100 / total).toInt()
            if (pct != m.lastPct) { m.lastPct = pct; emit(SessionEvent.SendProgress(m.id, m.offset.toLong(), total.toLong())) }
        }
        return p
    }

    private fun onAck(a: Packet.Ack) {
        val m = inFlight[a.msgId] ?: return
        when (a.status) {
            AckStatus.OK -> { inFlight.remove(m.id); emit(SessionEvent.Sent(m.id)) }
            AckStatus.CRC -> {
                if (m.crcResends >= CRC_RESENDS) failInFlight(m, FailReason.CRC, a.status)
                else { m.crcResends++; m.restart(0); m.restartOffset = null }
            }
            AckStatus.TOO_LARGE -> failInFlight(m, FailReason.TOO_LARGE, a.status)
            AckStatus.BUSY -> failInFlight(m, FailReason.BUSY, a.status)
            else -> failInFlight(m, FailReason.REJECTED, a.status) // 4 and reserved 5..255
        }
    }

    private fun onResume(r: Packet.Resume) {
        val m = inFlight[r.msgId] ?: return // unknown, finished or aborted: ignore
        if (r.offset > m.data.size) return
        val ro = m.restartOffset
        if (ro != null && r.offset <= ro) return // duplicate RESUME from the same gap
        if (m.restarts >= MAX_RESTARTS) {
            failInFlight(m, FailReason.LINK_UNRELIABLE); sendCancel(m.id, CancelReason.GAVE_UP); return
        }
        m.restarts++
        m.restart(r.offset.toInt())
        m.restartOffset = r.offset
    }

    private fun failInFlight(m: OutMsg, reason: FailReason, status: Int? = null) {
        inFlight.remove(m.id)
        emit(SessionEvent.Failed(m.id, reason, status))
    }

    private fun sendCancel(id: Int, reason: Int) = control.addLast { s -> Packet.Cancel(s, id, reason) }

    // ------------------------------------------------------------------ receiver

    private fun onSeqGap() {
        // section 8.1 / D2: resync every incomplete message
        for (m in incoming.values) {
            m.resync = true
            m.mismatchResumeSent = false
            m.lastActivity = clock()
            queueResume(m.id, m.received)
        }
    }

    private fun onMsgStart(p: Packet.MsgStart) {
        unknownResumed.remove(p.msgId)
        findStatus(completed, p.msgId)?.let { queueAck(p.msgId, it); return } // duplicate after lost ACK
        findStatus(rejected, p.msgId)?.let { queueAck(p.msgId, it); return }
        incoming.remove(p.msgId) // MSG_START for a message being received restarts it
        if (limits.maxMessage != 0xFFFFFFFFL && p.totalLen > limits.maxMessage || p.totalLen > Int.MAX_VALUE - 8) {
            reject(p.msgId, AckStatus.TOO_LARGE); return
        }
        if (incoming.size >= limits.window) { reject(p.msgId, AckStatus.BUSY); return }
        val m = InMsg(p.msgId, p.totalLen.toInt(), p.crc32, clock())
        incoming[m.id] = m
        accept(m, p.payload)
    }

    private fun onMsgData(id: Int, payload: ByteArray) {
        val m = incoming[id]
        if (m == null) { unknownData(id); return }
        if (m.resync) return
        accept(m, payload)
    }

    private fun onMsgResend(p: Packet.MsgResend) {
        val m = incoming[p.msgId]
        if (m == null) { unknownData(p.msgId); return }
        if (p.offset != m.received.toLong()) {
            m.resync = true
            if (!m.mismatchResumeSent) { m.mismatchResumeSent = true; queueResume(m.id, m.received) }
            return
        }
        m.resync = false
        accept(m, p.payload)
    }

    private fun unknownData(id: Int) {
        if (findStatus(completed, id) != null || findStatus(rejected, id) != null) return // stale packets
        if (unknownResumed.containsKey(id)) return
        unknownResumed[id] = clock()
        queueResume(id, 0) // its MSG_START was lost
    }

    private fun accept(m: InMsg, payload: ByteArray) {
        val n = payload.size
        m.lastActivity = clock()
        if (m.received.toLong() + n > m.total) { // corrupt sender
            incoming.remove(m.id); queueAck(m.id, AckStatus.CRC); return
        }
        payload.copyInto(m.buf, m.received)
        m.crcState = Crc32.update(m.crcState, payload)
        m.received += n
        if (n > 0) { m.idleResumes = 0; m.mismatchResumeSent = false }
        val pr = peer
        if (pr != null && pr.wantsProgress) {
            val mark = m.received / PROGRESS_STEP
            if (mark > m.progressMark && m.received < m.total) {
                m.progressMark = mark
                val id = m.id; val rcv = m.received.toLong()
                control.addLast { s -> Packet.Progress(s, id, rcv) }
            }
        }
        if (m.received < m.total) {
            val pct = (m.received.toLong() * 100 / m.total).toInt()
            if (pct != m.lastPct) { m.lastPct = pct; emit(SessionEvent.ReceiveProgress(m.id, m.received.toLong(), m.total.toLong())) }
            return
        }
        incoming.remove(m.id)
        val crc = Crc32.finish(m.crcState).toLong() and 0xFFFFFFFFL
        if (crc != m.crc) {
            // status 1 is not remembered, so the sender's automatic same-msgId retry is accepted (see NOTES.md)
            queueAck(m.id, AckStatus.CRC); return
        }
        remember(completed, m.id, AckStatus.OK, limits.window)
        queueAck(m.id, AckStatus.OK)
        emit(SessionEvent.Received(m.id, m.buf))
    }

    private fun reject(id: Int, status: Int) {
        remember(rejected, id, status, REJECTED_MEMORY)
        queueAck(id, status)
    }

    private fun queueAck(id: Int, status: Int) = control.addLast { s -> Packet.Ack(s, id, status) }
    private fun queueResume(id: Int, offset: Int) {
        val off = offset.toLong()
        control.addLast { s -> Packet.Resume(s, id, off) }
    }

    private fun remember(list: ArrayDeque<IntArray>, id: Int, status: Int, cap: Int) {
        list.removeAll { it[0] == id }
        list.addLast(intArrayOf(id, status))
        while (list.size > cap) list.removeFirst()
    }

    private fun findStatus(list: ArrayDeque<IntArray>, id: Int): Int? = list.firstOrNull { it[0] == id }?.get(1)

    private fun emit(e: SessionEvent) = listener(e)

    private class OutMsg(val id: Int, val data: ByteArray, val crc: Long) {
        var offset = 0
        var startPending = false
        var resendPending = false
        var sending = false
        var waitingSinceWrite = false
        var ackResends = 0
        var crcResends = 0
        var restarts = 0
        var restartOffset: Long? = null
        var lastActivity = 0L
        var lastPct = -1

        fun restart(at: Int) {
            offset = at
            startPending = at == 0
            resendPending = at != 0
            sending = true
            waitingSinceWrite = false
        }
    }

    private class InMsg(val id: Int, val total: Int, val crc: Long, now: Long) {
        val buf = ByteArray(total)
        var received = 0
        var crcState = Crc32.INITIAL
        var resync = false
        var mismatchResumeSent = false
        var idleResumes = 0
        var lastActivity = now
        var progressMark = 0
        var lastPct = -1
    }

    companion object {
        const val MIN_PACKET = 20
        const val MAX_PACKET = 512
        const val HELLO_RESENDS = 3
        const val ACK_RESENDS = 3
        const val CRC_RESENDS = 3
        const val MAX_RESTARTS = 8
        const val IDLE_RESUMES = 3
        const val PROGRESS_STEP = 65536
        private const val REJECTED_MEMORY = 16
    }
}
