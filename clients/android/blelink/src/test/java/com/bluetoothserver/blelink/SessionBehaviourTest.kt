package com.bluetoothserver.blelink

import com.bluetoothserver.blelink.codec.Crc32
import com.bluetoothserver.blelink.codec.Packet
import com.bluetoothserver.blelink.codec.PacketCodec
import com.bluetoothserver.blelink.session.FailReason
import com.bluetoothserver.blelink.session.LocalLimits
import com.bluetoothserver.blelink.session.SessionEvent
import com.bluetoothserver.blelink.session.SessionState
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** Session rules from PROTOCOL.md that the vectors do not cover. */
class SessionBehaviourTest {

    private fun decode(b: ByteArray) = PacketCodec.decode(b)!!
    private fun Harness.packets() = drain().map { decode(it) }

    @Test
    fun handshakeAndPacketSize() {
        val h = Harness(osLimit = 244)
        assertEquals(SessionState.OPENING, h.session.state)
        assertEquals(20, h.session.sendPacketSize)
        val hello = decode(h.drain().single()) as Packet.Hello
        assertEquals(244, hello.maxPacket)
        assertEquals(0, hello.seq)
        h.feed(Packet.Hello(0, 1, 182, 1000, 2, 0, 0))
        assertEquals(SessionState.READY, h.session.state)
        assertEquals(182, h.session.sendPacketSize) // min(peer maxPacket, osLimit)
        assertEquals(182, h.ofType<SessionEvent.PeerReady>().single().sendPacketSize)
        // peer maxPacket outside 20..512 is clamped
        val h2 = Harness(osLimit = 517); h2.handshake(maxPacket = 9999)
        assertEquals(512, h2.session.sendPacketSize)
        val h3 = Harness(osLimit = 244); h3.handshake(maxPacket = 3)
        assertEquals(20, h3.session.sendPacketSize)
    }

    @Test
    fun timeoutIsMaxOfBothSides() {
        val h = Harness(LocalLimits(timeout = 8))
        h.handshake(timeout = 0)
        assertEquals(10_000, h.session.timeoutMs)
        val h2 = Harness(LocalLimits(timeout = 8)); h2.handshake(timeout = 120)
        assertEquals(30_000, h2.session.timeoutMs)
    }

    @Test
    fun messagesWaitForReadyAndRespectPeerWindow() {
        val h = Harness()
        val a = h.session.send("a".toByteArray())
        val b = h.session.send("b".toByteArray())
        assertEquals(0, a); assertEquals(1, b)
        assertEquals(1, h.drain().size) // only HELLO before READY
        h.feed(Packet.Hello(0, 1, 244, 1000, 1, 0, 0))
        assertEquals(listOf(0), h.packets().map { (it as Packet.MsgStart).msgId })
        h.feed(Packet.Ack(1, 0, 0))
        assertEquals(listOf(1), h.packets().map { (it as Packet.MsgStart).msgId })
        h.feed(Packet.Ack(2, 1, 0))
        assertEquals(listOf(SessionEvent.Sent(0), SessionEvent.Sent(1)), h.ofType<SessionEvent.Sent>())
    }

    @Test
    fun messageLargerThanPeerMaxMessageFailsLocally() {
        val h = Harness(); h.handshake(maxMessage = 10)
        val id = h.session.send(ByteArray(11))
        assertTrue(h.drain().isEmpty())
        assertEquals(listOf(SessionEvent.Failed(id, FailReason.TOO_LARGE)), h.ofType<SessionEvent.Failed>())
    }

    @Test
    fun emptyMessage() {
        val h = Harness(); h.handshake()
        h.session.send(ByteArray(0))
        assertEquals("020100000000000000000000", h.drain().single().hex())
        h.feed("020102000000000000000000")
        assertEquals(0, h.ofType<SessionEvent.Received>().single().data.size)
    }

    @Test
    fun roundRobinBetweenInFlightMessages() {
        val h = Harness(osLimit = 20); h.handshake(maxPacket = 20)
        h.session.send(ByteArray(40)); h.session.send(ByteArray(40))
        val ids = h.packets().map {
            when (it) { is Packet.MsgStart -> it.msgId; is Packet.MsgData -> it.msgId; else -> -1 }
        }
        assertEquals(listOf(0, 1, 0, 1, 0, 1), ids)
    }

    @Test
    fun ackTimeoutResendsSameMsgIdThreeTimesThenFails() {
        val h = Harness(); h.handshake()
        val id = h.session.send("x".toByteArray())
        assertEquals(1, h.drain().size)
        repeat(3) { n ->
            h.now += 9_999; h.session.onTick(); assertTrue(h.drain().isEmpty())
            h.now += 1; h.session.onTick()
            val p = h.packets().single() as Packet.MsgStart
            assertEquals(id, p.msgId)
            assertEquals(n + 2, p.seq)
        }
        h.now += 10_000; h.session.onTick()
        assertEquals(listOf(SessionEvent.Failed(id, FailReason.TIMEOUT)), h.ofType<SessionEvent.Failed>())
        assertEquals(Packet.Cancel(5, id, 1), h.packets().single())
    }

    @Test
    fun crcAckRetriesThreeTimesThenFails() {
        val h = Harness(); h.handshake()
        val id = h.session.send("x".toByteArray()); h.drain()
        var seq = 1
        repeat(3) {
            h.feed(Packet.Ack(seq++, id, 1))
            assertTrue(h.packets().single() is Packet.MsgStart)
        }
        h.feed(Packet.Ack(seq, id, 1))
        assertEquals(listOf(SessionEvent.Failed(id, FailReason.CRC, 1)), h.ofType<SessionEvent.Failed>())
        assertTrue(h.drain().isEmpty())
    }

    @Test
    fun earlyAckStopsTheMessage() {
        for ((status, reason) in listOf(2 to FailReason.TOO_LARGE, 3 to FailReason.BUSY, 4 to FailReason.REJECTED, 77 to FailReason.REJECTED)) {
            val h = Harness(osLimit = 20); h.handshake(maxPacket = 20)
            val id = h.session.send(ByteArray(100))
            assertTrue(h.session.poll() != null)
            h.feed(Packet.Ack(1, id, status))
            assertTrue(h.drain().isEmpty())
            assertEquals(listOf(SessionEvent.Failed(id, reason, status)), h.ofType<SessionEvent.Failed>())
        }
    }

    @Test
    fun okAckWhileStillSendingCompletesTheMessage() {
        // e.g. a stored ACK re-sent by the receiver right after our retried MSG_START
        val h = Harness(osLimit = 20); h.handshake(maxPacket = 20)
        val id = h.session.send(ByteArray(100))
        h.session.poll()
        h.feed(Packet.Ack(1, id, 0))
        assertTrue(h.drain().isEmpty())
        assertEquals(listOf(SessionEvent.Sent(id)), h.ofType<SessionEvent.Sent>())
    }

    @Test
    fun duplicateResumeIsIgnoredUntilNextPacketAndRestartsAreCapped() {
        val h = Harness(osLimit = 20); h.handshake(maxPacket = 20)
        val id = h.session.send(testPayload(200))
        h.drain()
        var seq = 1
        h.feed(Packet.Resume(seq++, id, 24))
        h.feed(Packet.Resume(seq++, id, 24)) // same gap, nothing sent since the restart: ignored
        val repaired = h.packets()
        assertEquals(24L, (repaired.first() as Packet.MsgResend).offset)
        assertEquals(1, repaired.count { it is Packet.MsgResend })
        repeat(7) { h.feed(Packet.Resume(seq++, id, 0)); h.drain() } // 8 restarts in total
        assertTrue(h.ofType<SessionEvent.Failed>().isEmpty())
        h.feed(Packet.Resume(seq, id, 0))
        assertEquals(listOf(SessionEvent.Failed(id, FailReason.LINK_UNRELIABLE)), h.ofType<SessionEvent.Failed>())
        assertEquals(Packet.Cancel(h.lastSeqPlusOne(), id, 1), h.packets().single())
    }

    private fun Harness.lastSeqPlusOne(): Int {
        // seq of the next packet = number of packets sent since HELLO + 1; recomputed from a probe-free count
        return sentSinceHello + 1
    }

    @Test
    fun resumeForUnknownIdIsIgnored() {
        val h = Harness(); h.handshake()
        h.feed(Packet.Resume(1, 42, 0))
        assertTrue(h.drain().isEmpty())
    }

    @Test
    fun cancelByApplication() {
        val h = Harness(osLimit = 20); h.handshake(maxPacket = 20)
        val id = h.session.send(ByteArray(100))
        h.session.poll()
        assertTrue(h.session.cancel(id))
        assertEquals(listOf(Packet.Cancel(2, id, 0)), h.packets())
        assertEquals(listOf(SessionEvent.Failed(id, FailReason.CANCELLED)), h.ofType<SessionEvent.Failed>())
    }

    // ---------------------------------------------------------------- receiver

    private fun start(seq: Int, id: Int, data: ByteArray, n: Int = data.size) =
        Packet.MsgStart(seq, id, data.size.toLong(), Crc32.of(data), data.copyOf(minOf(n, data.size)))

    @Test
    fun duplicateMsgStartAfterCompletionIsReAckedNotRedelivered() {
        val h = Harness(); h.handshake()
        h.feed(start(1, 7, "hi".toByteArray()))
        h.feed(start(2, 7, "hi".toByteArray()))
        assertEquals(1, h.ofType<SessionEvent.Received>().size)
        assertEquals(listOf(Packet.Ack(1, 7, 0), Packet.Ack(2, 7, 0)), h.packets())
    }

    @Test
    fun crcMismatchIsAckedWithStatus1AndRetryIsAccepted() {
        val h = Harness(); h.handshake()
        val good = start(1, 3, "hello".toByteArray())
        h.feed(Packet.MsgStart(1, 3, 5, good.crc32 xor 1, good.payload))
        assertTrue(h.ofType<SessionEvent.Received>().isEmpty())
        h.feed(Packet.MsgStart(2, 3, 5, good.crc32, good.payload))
        assertEquals(1, h.ofType<SessionEvent.Received>().size)
        assertEquals(listOf(Packet.Ack(1, 3, 1), Packet.Ack(2, 3, 0)), h.packets())
    }

    @Test
    fun tooLargeAndWindowExceededAreRejectedEarly() {
        val h = Harness(LocalLimits(maxMessage = 50, window = 1)); h.handshake()
        h.feed(start(1, 1, ByteArray(51), 8))
        h.feed(Packet.MsgData(2, 1, ByteArray(16))) // ignored silently, no RESUME
        h.feed(start(3, 2, ByteArray(40), 8))       // occupies the single window slot
        h.feed(start(4, 3, ByteArray(40), 8))       // window exceeded
        assertEquals(listOf(Packet.Ack(1, 1, 2), Packet.Ack(2, 3, 3)), h.packets())
    }

    @Test
    fun overflowingPayloadFailsWithCrcStatus() {
        val h = Harness(); h.handshake()
        h.feed(Packet.MsgStart(1, 1, 4, 0, ByteArray(4 + 1)))
        assertEquals(listOf(Packet.Ack(1, 1, 1)), h.packets())
    }

    @Test
    fun dataForUnknownMessageGetsOneResumeAtZero() {
        val h = Harness(); h.handshake()
        h.feed(Packet.MsgData(1, 9, ByteArray(4)))
        h.feed(Packet.MsgData(2, 9, ByteArray(4)))
        assertEquals(listOf(Packet.Resume(1, 9, 0)), h.packets())
        // after T it may ask again
        h.now += 10_000; h.session.onTick()
        h.feed(Packet.MsgData(3, 9, ByteArray(4)))
        assertEquals(listOf(Packet.Resume(2, 9, 0)), h.packets())
    }

    @Test
    fun resendWithWrongOffsetTriggersOneResume() {
        val h = Harness(); h.handshake()
        val data = testPayload(100)
        h.feed(start(1, 4, data, 8))
        h.feed(Packet.MsgData(3, 4, data.copyOfRange(8, 24))) // gap: RESUME 4@8, data dropped
        h.feed(Packet.MsgResend(4, 4, 30, data.copyOfRange(30, 40)))
        h.feed(Packet.MsgResend(5, 4, 31, data.copyOfRange(31, 40)))
        assertEquals(listOf(Packet.Resume(1, 4, 8), Packet.Resume(2, 4, 8)), h.packets())
        h.feed(Packet.MsgResend(6, 4, 8, data.copyOfRange(8, 50)))
        h.feed(Packet.MsgData(7, 4, data.copyOfRange(50, 100)))
        assertArrayEquals(data, h.ofType<SessionEvent.Received>().single().data)
    }

    @Test
    fun restartingMsgStartDiscardsPartialState() {
        val h = Harness(); h.handshake()
        val data = testPayload(40)
        h.feed(start(1, 4, data, 8))
        h.feed(Packet.MsgData(2, 4, data.copyOfRange(8, 20)))
        h.feed(start(3, 4, data, 8))
        h.feed(Packet.MsgData(4, 4, data.copyOfRange(8, 40)))
        assertArrayEquals(data, h.ofType<SessionEvent.Received>().single().data)
    }

    @Test
    fun idleTimeoutSendsResumeThenGivesUpAfterThree() {
        val h = Harness(); h.handshake()
        h.feed(start(1, 4, ByteArray(100), 8))
        repeat(3) {
            h.now += 10_000; h.session.onTick()
            assertEquals(listOf(Packet.Resume(it + 1, 4, 8)), h.packets())
        }
        h.now += 10_000; h.session.onTick()
        assertTrue(h.drain().isEmpty())
        // the message is gone: its data now counts as unknown
        h.feed(Packet.MsgData(2, 4, ByteArray(4)))
        assertEquals(listOf(Packet.Resume(4, 4, 0)), h.packets())
    }

    @Test
    fun peerCancelFreesTheMessage() {
        val h = Harness(LocalLimits(window = 1)); h.handshake()
        h.feed(start(1, 4, ByteArray(100), 8))
        h.feed(Packet.Cancel(2, 4, 0))
        h.feed(start(3, 5, "ok".toByteArray()))
        assertEquals(listOf(Packet.Ack(1, 5, 0)), h.packets())
    }

    @Test
    fun progressIsSentWhenPeerAsksForIt() {
        val data = testPayload(140_000)
        for (flags in listOf(1, 0)) {
            val h = Harness(osLimit = 512); h.handshake(flags = flags)
            h.feed(start(1, 2, data, 500))
            var off = 500; var seq = 2
            while (off < data.size) { val n = minOf(508, data.size - off); h.feed(Packet.MsgData(seq++, 2, data.copyOfRange(off, off + n))); off += n }
            val out = h.packets()
            if (flags == 1) {
                val pr = out.filterIsInstance<Packet.Progress>()
                assertEquals(2, pr.size)
                assertTrue(pr[0].bytesReceived >= 65536 && pr[1].bytesReceived >= 131072)
            } else assertTrue(out.none { it is Packet.Progress })
            assertEquals(Packet.Ack(out.size, 2, 0), out.last())
        }
    }

    @Test
    fun seqWrapIsNotAGap() {
        val h = Harness(osLimit = 20); h.handshake()
        val data = testPayload(8 + 16 * 300)
        h.feed(start(1, 3, data, 8))
        var off = 8; var seq = 2
        while (off < data.size) { h.feed(Packet.MsgData(seq and 0xFF, 3, data.copyOfRange(off, off + 16))); off += 16; seq++ }
        assertArrayEquals(data, h.ofType<SessionEvent.Received>().single().data)
        assertTrue(h.packets().none { it is Packet.Resume })
    }

    @Test
    fun unknownPacketTypeCountsSeq() {
        val h = Harness(); h.handshake()
        h.session.onPacket(byteArrayOf(0x42, 1))
        h.feed(start(2, 1, "a".toByteArray()))
        assertEquals(listOf(Packet.Ack(1, 1, 0)), h.packets())
    }

    @Test
    fun packetsBeforeReadyAreIgnored() {
        val h = Harness()
        h.drain()
        h.feed(start(1, 1, "a".toByteArray()))
        assertTrue(h.drain().isEmpty())
        assertTrue(h.ofType<SessionEvent.Received>().isEmpty())
    }

    // ---------------------------------------------------------------- session lifecycle

    @Test
    fun peerHelloWhileReadyResetsAndIsAnsweredOnce() {
        val h = Harness(osLimit = 20); h.handshake(maxPacket = 20)
        val id = h.session.send(ByteArray(100))
        h.session.poll()
        h.feed(start(1, 9, ByteArray(100), 8)) // partial incoming
        h.feed(Packet.Hello(0, 1, 20, 1000, 4, 0, 0))
        assertEquals(listOf(SessionEvent.Failed(id, FailReason.RESET)), h.ofType<SessionEvent.Failed>())
        val out = h.packets()
        assertEquals(1, out.size)
        assertTrue(out[0] is Packet.Hello)
        // after the reset our seq restarts at 1 and the old partial message is gone
        h.session.send("n".toByteArray())
        assertEquals(1, h.packets().single().seq)
        h.feed(Packet.MsgData(1, 9, ByteArray(4)))
        assertEquals(listOf(Packet.Resume(2, 9, 0)), h.packets())
        assertTrue(h.ofType<SessionEvent.PeerReady>().last().reset)
    }

    @Test
    fun lateSecondHelloReplyDoesNotStartAHelloPingPong() {
        val h = Harness()
        h.drain()                       // HELLO #1
        h.now += 10_000; h.session.onTick()
        h.drain()                       // HELLO #2 (timeout resend)
        h.feed(Packet.Hello(0, 1, 244, 1000, 4, 0, 0)) // reply to #1
        h.feed(Packet.Hello(0, 1, 244, 1000, 4, 0, 0)) // reply to #2
        assertTrue(h.drain().isEmpty())
        assertEquals(1, h.ofType<SessionEvent.PeerReady>().size)
    }

    @Test
    fun helloIsResentThreeTimesThenHandshakeFails() {
        val h = Harness()
        val id = h.session.send("x".toByteArray())
        assertEquals(1, h.drain().size)
        repeat(3) { h.now += 10_000; h.session.onTick(); assertTrue(h.packets().single() is Packet.Hello) }
        h.now += 10_000; h.session.onTick()
        assertEquals(SessionState.CLOSED, h.session.state)
        assertTrue(h.events.contains(SessionEvent.HandshakeFailed))
        assertEquals(listOf(SessionEvent.Failed(id, FailReason.DISCONNECTED)), h.ofType<SessionEvent.Failed>())
    }

    @Test
    fun disconnectFailsEverything() {
        val h = Harness(osLimit = 20); h.handshake(maxPacket = 20, window = 1)
        val a = h.session.send(ByteArray(100)); val b = h.session.send(ByteArray(1))
        h.session.poll()
        h.session.onDisconnected()
        assertEquals(setOf(a, b), h.ofType<SessionEvent.Failed>().map { it.msgId }.toSet())
        assertTrue(h.ofType<SessionEvent.Failed>().all { it.reason == FailReason.DISCONNECTED })
        assertEquals(SessionState.CLOSED, h.session.state)
    }

    @Test
    fun peerProgressIsReported() {
        val h = Harness(osLimit = 20); h.handshake(maxPacket = 20)
        val id = h.session.send(ByteArray(100)); h.drain()
        h.feed(Packet.Progress(1, id, 64))
        assertEquals(listOf(SessionEvent.PeerProgress(id, 64, 100)), h.ofType<SessionEvent.PeerProgress>())
        assertEquals(100L, h.ofType<SessionEvent.SendProgress>().last().bytesSent)
    }
}
