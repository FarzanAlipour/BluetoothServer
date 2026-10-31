package com.bluetoothserver.blelink

import com.bluetoothserver.blelink.session.BleLinkSession
import com.bluetoothserver.blelink.session.FailReason
import com.bluetoothserver.blelink.session.LocalLimits
import com.bluetoothserver.blelink.session.SessionEvent
import com.bluetoothserver.blelink.transport.PacketTransport
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.async
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.filterIsInstance
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.toList
import kotlinx.coroutines.flow.take
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.CoroutineStart
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test
import java.util.Collections
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/**
 * [BleLinkClient] end to end over a fake [PacketTransport] whose far end is a second [BleLinkSession]
 * playing the server (the session is symmetric apart from who sends HELLO first). Exercises the client's
 * one-write-at-a-time pump, threading and event mapping without Bluetooth. Optionally drops one client
 * packet to exercise RESUME through the full stack.
 */
class BleLinkClientLoopbackTest {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    @After fun tearDown() = scope.cancel()

    /** Simulated link. Everything on the "air" runs on one executor thread (like a GATT callback thread). */
    private class FakeLink(val osLimit: Int, val dropClientPacketIndex: Int = -1) : PacketTransport {
        val air = Executors.newSingleThreadExecutor()
        lateinit var client: PacketTransport.Listener
        val serverEvents: MutableList<SessionEvent> = Collections.synchronizedList(mutableListOf())
        val server = BleLinkSession(LocalLimits(maxPacket = osLimit, window = 4)) { serverEvents += it }
        var outstanding = 0
        var maxOutstanding = 0
        private var clientPackets = 0
        private var serverWriting = false

        override fun start(listener: PacketTransport.Listener) {
            client = listener
            air.execute {
                server.open(osLimit)
                server.poll() // the server's own HELLO is held back: it is sent as the answer below
                server.onPacketWritten()
                client.onLinkReady(osLimit)
            }
        }

        override fun write(bytes: ByteArray, withResponse: Boolean) {
            if (air.isShutdown) return
            synchronized(this) { outstanding++; maxOutstanding = maxOf(maxOutstanding, outstanding) }
            air.execute {
                val index = clientPackets++
                if (bytes[0].toInt() == 0x01) {
                    // client HELLO: the server answers with its HELLO
                    server.onPacket(bytes)
                    client.onPacket(helloOf(server))
                } else if (index != dropClientPacketIndex) {
                    server.onPacket(bytes)
                }
                synchronized(this) { outstanding-- }
                client.onWriteComplete(true)
                pumpServer()
            }
        }

        fun serverSend(data: ByteArray) = air.execute { server.send(data); pumpServer() }
        fun serverTick() = air.execute { server.onTick(); pumpServer() }

        private fun pumpServer() {
            if (serverWriting) return
            serverWriting = true
            while (true) {
                val o = server.poll() ?: break
                server.onPacketWritten()
                client.onPacket(o.bytes)
            }
            serverWriting = false
        }

        override fun close() = air.execute { client.onDisconnected(null, "closed by test"); air.shutdown() }

        private fun helloOf(s: BleLinkSession): ByteArray {
            // Re-encode the server HELLO from its limits (it was polled at open()).
            val p = com.bluetoothserver.blelink.codec.Packet.Hello(0, 1, osLimit, 16L * 1024 * 1024, 4, 1, 0)
            return com.bluetoothserver.blelink.codec.PacketCodec.encode(p)
        }
    }

    @Test
    fun bothDirectionsOverTheClientApi() = runBlocking<Unit> {
        val link = FakeLink(osLimit = 182)
        val client = BleLinkClient(link, scope = scope, tickMs = 20)
        val received = scope.eager { client.events.filterIsInstance<BleLinkEvent.Received>().first() }
        val sent = scope.eager { client.events.filterIsInstance<BleLinkEvent.Sent>().take(2).toList() }
        client.connect()
        withTimeout(5_000) {
            val peer = client.awaitReady()
            assertEquals(182, peer.maxPacket)
            assertEquals(182, client.sendPacketSize)
            val big = testPayload(50_000)
            val a = client.send("hello".toByteArray())
            val b = client.send(big)
            link.serverSend(testPayload(3_000))
            assertEquals(listOf(a, b).toSet(), sent.await().map { it.msgId }.toSet())
            assertArrayEquals(testPayload(3_000), received.await().data)
            val serverGot = link.serverEvents.filterIsInstance<SessionEvent.Received>()
            assertArrayEquals(big, serverGot.single { it.msgId == b }.data)
        }
        assertEquals(1, link.maxOutstanding) // never more than one write in flight
        client.disconnect()
        withTimeout(5_000) { client.state.first { it == LinkState.DISCONNECTED } }
    }

    @Test
    fun lostClientPacketIsRepairedThroughResume() = runBlocking<Unit> {
        val link = FakeLink(osLimit = 20, dropClientPacketIndex = 3) // HELLO, START, DATA, [lost DATA]
        val client = BleLinkClient(link, scope = scope, tickMs = 20)
        val sent = scope.eager { client.events.filterIsInstance<BleLinkEvent.Sent>().first() }
        client.connect()
        withTimeout(5_000) {
            client.awaitReady()
            val id = client.send(testPayload(400))
            assertEquals(id, sent.await().msgId)
        }
        assertArrayEquals(testPayload(400), link.serverEvents.filterIsInstance<SessionEvent.Received>().single().data)
        client.disconnect()
    }

    @Test
    fun disconnectFailsUnfinishedSends() = runBlocking<Unit> {
        val link = FakeLink(osLimit = 20)
        val client = BleLinkClient(link, scope = scope, tickMs = 20)
        val events = Collections.synchronizedList(mutableListOf<BleLinkEvent>())
        val collector = scope.launch(start = CoroutineStart.UNDISPATCHED) { client.events.collect { events += it } }
        client.connect()
        withTimeout(5_000) { client.awaitReady() }
        // pause the "air" so the message cannot complete, then drop the link
        val gate = java.util.concurrent.CountDownLatch(1)
        link.air.execute { gate.await(5, TimeUnit.SECONDS) }
        val id = client.send(testPayload(10_000))
        link.close()
        gate.countDown()
        withTimeout(5_000) { client.state.first { it == LinkState.DISCONNECTED } }
        withTimeout(5_000) { while (events.none { it is BleLinkEvent.Disconnected }) kotlinx.coroutines.delay(10) }
        assertEquals(BleLinkEvent.Failed(id, FailReason.DISCONNECTED), events.filterIsInstance<BleLinkEvent.Failed>().single())
        collector.cancel()
    }
}

private fun <T> CoroutineScope.eager(block: suspend CoroutineScope.() -> T) =
    async(start = CoroutineStart.UNDISPATCHED, block = block)
