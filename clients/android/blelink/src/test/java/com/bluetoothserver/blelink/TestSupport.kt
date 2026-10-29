package com.bluetoothserver.blelink

import com.bluetoothserver.blelink.codec.Packet
import com.bluetoothserver.blelink.codec.PacketCodec
import com.bluetoothserver.blelink.session.BleLinkSession
import com.bluetoothserver.blelink.session.LocalLimits
import com.bluetoothserver.blelink.session.SessionEvent
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

object Vectors {
    val json: JSONObject by lazy {
        val path = System.getProperty("blelink.vectors") ?: "../../../docs/protocol-test-vectors.json"
        JSONObject(File(path).readText())
    }
}

fun hex(s: String): ByteArray = ByteArray(s.length / 2) { s.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
fun ByteArray.hex(): String = joinToString("") { "%02x".format(it.toInt() and 0xFF) }
fun JSONArray.strings(): List<String> = (0 until length()).map { getString(it) }

/** payloadRule from the vectors: byte[i] = (i * 7 + 3) mod 256 */
fun testPayload(n: Int) = ByteArray(n) { ((it * 7 + 3) and 0xFF).toByte() }

/** A client session with a fake clock and an event log. */
class Harness(
    limits: LocalLimits = LocalLimits(),
    osLimit: Int = 244,
) {
    var now = 1_000L
    val events = mutableListOf<SessionEvent>()
    val session = BleLinkSession(limits, { now }) { events += it }
    init { session.open(osLimit) }

    /** Packets written through [drain] since our last HELLO (the next packet's seq is this + 1). */
    var sentSinceHello = 0
        private set

    /** Polls everything that is ready to write, simulating a write completion after each packet. */
    fun drain(max: Int = 100_000): List<ByteArray> {
        val out = mutableListOf<ByteArray>()
        while (out.size < max) {
            val o = session.poll() ?: break
            out += o.bytes
            if (o.bytes[0].toInt() == 0x01) sentSinceHello = 0 else sentSinceHello++
            session.onPacketWritten()
        }
        return out
    }

    fun feed(hexPacket: String) = session.onPacket(hex(hexPacket))
    fun feed(p: Packet) = session.onPacket(PacketCodec.encode(p))

    /** Writes our HELLO and answers with a server HELLO. */
    fun handshake(
        maxPacket: Int = 512, maxMessage: Long = 16L * 1024 * 1024, window: Int = 4, flags: Int = 1, timeout: Int = 0,
    ): ByteArray {
        val hello = drain().single()
        feed(Packet.Hello(0, 1, maxPacket, maxMessage, window, flags, timeout))
        return hello
    }

    inline fun <reified T : SessionEvent> ofType(): List<T> = events.filterIsInstance<T>()
}
