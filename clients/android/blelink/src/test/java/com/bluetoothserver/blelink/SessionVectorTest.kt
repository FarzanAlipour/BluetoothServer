package com.bluetoothserver.blelink

import com.bluetoothserver.blelink.session.LocalLimits
import com.bluetoothserver.blelink.session.SessionEvent
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The session (both directions) against docs/protocol-test-vectors.json:
 * messageSplits, boundaryCases20, interleaving, resume, HELLO client.
 */
class SessionVectorTest {

    private val v = Vectors.json

    @Test
    fun clientHelloMatchesVector() {
        val h = Harness(LocalLimits(timeout = 40), osLimit = 244)
        val hello = h.drain().single()
        assertEquals(v.getJSONArray("packets").getJSONObject(0).getString("hex"), hello.hex())
    }

    @Test
    fun senderSplitsMatchVectors() {
        val cases = v.getJSONObject("messageSplits").getJSONArray("cases")
        assertEquals(4, cases.length())
        for (i in 0 until cases.length()) {
            val c = cases.getJSONObject(i)
            val ps = c.getInt("packetSize")
            val h = Harness(osLimit = ps)
            h.handshake(maxPacket = 512)
            assertEquals(ps, h.session.sendPacketSize)
            h.session.nextMsgId = c.getInt("msgId")
            val id = h.session.send(testPayload(c.getInt("messageLength")))
            assertEquals(c.getInt("msgId"), id)
            val out = h.drain().map { it.hex() }
            assertEquals("packetSize $ps", c.getJSONArray("packetsHex").strings(), out)
            assertEquals(c.getInt("packetCount"), out.size)
            h.feed("04010201" + "00") // server ACK seq 1, msgId 258, OK
            assertEquals(listOf(SessionEvent.Sent(258)), h.ofType<SessionEvent.Sent>())
        }
    }

    @Test
    fun receiverReassemblesSplitVectors() {
        val cases = v.getJSONObject("messageSplits").getJSONArray("cases")
        for (i in 0 until cases.length()) {
            val c = cases.getJSONObject(i)
            val h = Harness(osLimit = 512)
            h.handshake()
            c.getJSONArray("packetsHex").strings().forEach { h.feed(it) }
            val rx = h.ofType<SessionEvent.Received>().single()
            assertEquals(258, rx.msgId)
            assertArrayEquals(testPayload(1000), rx.data)
            assertEquals(listOf("0401020100"), h.drain().map { it.hex() })
        }
    }

    @Test
    fun boundaryCasesAtPacketSize20() {
        val cases = v.getJSONObject("boundaryCases20").getJSONArray("cases")
        for (i in 0 until cases.length()) {
            val c = cases.getJSONObject(i)
            val n = c.getInt("messageLength")
            val expected = c.getJSONArray("packetsHex").strings()
            // send
            val s = Harness(osLimit = 20)
            s.handshake(maxPacket = 20)
            s.session.nextMsgId = 5
            s.session.send(testPayload(n))
            assertEquals("len $n", expected, s.drain().map { it.hex() })
            // receive
            val r = Harness(osLimit = 20)
            r.handshake()
            expected.forEach { r.feed(it) }
            assertArrayEquals(testPayload(n), r.ofType<SessionEvent.Received>().single().data)
            assertEquals(listOf("0401050000"), r.drain().map { it.hex() })
        }
    }

    @Test
    fun interleavedMessagesAreDeliveredInCompletionOrder() {
        val iv = v.getJSONObject("interleaving")
        val h = Harness(osLimit = 20)
        h.handshake()
        iv.getJSONArray("packetsHex").strings().forEach { h.feed(it) }
        val rx = h.ofType<SessionEvent.Received>()
        val order = iv.getJSONArray("deliveryOrder")
        assertEquals((0 until order.length()).map { order.getInt(it) }, rx.map { it.msgId })
        assertArrayEquals("ping".toByteArray(), rx[0].data)
        assertArrayEquals(testPayload(60), rx[1].data)
        assertEquals(iv.getJSONArray("ackPacketsHex").strings(), h.drain().map { it.hex() })
    }

    @Test
    fun receiverRepairsSeqGapWithResumeAndResend() {
        val rv = v.getJSONObject("resume")
        val original = rv.getJSONArray("originalPacketsHex").strings()
        val lost = rv.getInt("lostIndex")
        val h = Harness(osLimit = 20)
        h.handshake()
        original.filterIndexed { i, _ -> i != lost }.forEach { h.feed(it) }
        assertEquals(listOf(rv.getString("resumeHex")), h.drain().map { it.hex() })
        assertTrue(h.ofType<SessionEvent.Received>().isEmpty())
        // The sender's next seq after 1..5 is 6. The vector numbers the repair 7..9 (see NOTES.md);
        // renumber to a gap-free stream here, and test the vector bytes as-is below.
        rv.getJSONArray("senderResendPacketsHex").strings().forEachIndexed { i, p -> h.feed(withSeq(p, 6 + i)) }
        assertArrayEquals(testPayload(60), h.ofType<SessionEvent.Received>().single().data)
        assertEquals(listOf("0402140000"), h.drain().map { it.hex() })
    }

    @Test
    fun receiverRepairsWithVectorBytesAsIs() {
        val rv = v.getJSONObject("resume")
        val original = rv.getJSONArray("originalPacketsHex").strings()
        val h = Harness(osLimit = 20)
        h.handshake()
        original.filterIndexed { i, _ -> i != rv.getInt("lostIndex") }.forEach { h.feed(it) }
        h.drain()
        rv.getJSONArray("senderResendPacketsHex").strings().forEach { h.feed(it) }
        assertArrayEquals(testPayload(60), h.ofType<SessionEvent.Received>().single().data)
        // seq 6 is missing in the vector, so the receiver reports a second gap (RESUME 20@24) before the ACK
        assertEquals(listOf("0502140018000000", "0403140000"), h.drain().map { it.hex() })
    }

    @Test
    fun senderAnswersResumeWithMsgResend() {
        val rv = v.getJSONObject("resume")
        val h = Harness(osLimit = 20)
        h.handshake(maxPacket = 20)
        h.session.nextMsgId = 20
        h.session.send(testPayload(60))
        assertEquals(rv.getJSONArray("originalPacketsHex").strings(), h.drain().map { it.hex() })
        h.feed(rv.getString("resumeHex"))
        val repaired = h.drain()
        val expected = rv.getJSONArray("senderResendPacketsHex").strings()
        assertEquals(expected.size, repaired.size)
        repaired.forEachIndexed { i, p ->
            assertEquals(6 + i, p[1].toInt() and 0xFF) // contiguous seq after 1..5
            assertEquals(expected[i], withSeq(p.hex(), 7 + i)) // vector bytes, vector seq numbering
        }
    }

    private fun withSeq(hexPacket: String, seq: Int) = hexPacket.substring(0, 2) + "%02x".format(seq) + hexPacket.substring(4)
}
