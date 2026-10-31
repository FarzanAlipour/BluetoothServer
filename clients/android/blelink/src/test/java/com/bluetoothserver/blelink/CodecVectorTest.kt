package com.bluetoothserver.blelink

import com.bluetoothserver.blelink.codec.Crc32
import com.bluetoothserver.blelink.codec.InfoCodec
import com.bluetoothserver.blelink.codec.Packet
import com.bluetoothserver.blelink.codec.PacketCodec
import org.json.JSONObject
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** docs/protocol-test-vectors.json: crc32, packets, info. */
class CodecVectorTest {

    @Test
    fun crc32Cases() {
        val cases = Vectors.json.getJSONObject("crc32").getJSONArray("cases")
        assertTrue(cases.length() > 0)
        for (i in 0 until cases.length()) {
            val c = cases.getJSONObject(i)
            val data = if (c.has("inputAscii")) c.getString("inputAscii").toByteArray(Charsets.US_ASCII) else hex(c.getString("inputHex"))
            assertEquals(c.toString(), c.getString("crc32"), "%08x".format(Crc32.of(data)))
        }
    }

    @Test
    fun crc32IsStreamable() {
        val data = testPayload(1000)
        var s = Crc32.INITIAL
        var o = 0
        for (n in listOf(1, 7, 100, 0, 892)) { s = Crc32.update(s, data, o, n); o += n }
        assertEquals(Crc32.of(data), Crc32.finish(s).toLong() and 0xFFFFFFFFL)
    }

    @Test
    fun everySinglePacketDecodesToItsFieldsAndReEncodesToItsBytes() {
        val packets = Vectors.json.getJSONArray("packets")
        assertTrue(packets.length() >= 12)
        for (i in 0 until packets.length()) {
            val v = packets.getJSONObject(i)
            val bytes = hex(v.getString("hex"))
            assertEquals(v.getString("name"), v.getInt("length"), bytes.size)
            val p = PacketCodec.decode(bytes)!!
            checkFields(v.getString("name"), p, v.getJSONObject("fields"))
            assertArrayEquals(v.getString("name"), bytes, PacketCodec.encode(p))
        }
    }

    private fun checkFields(name: String, p: Packet, f: JSONObject) {
        assertEquals(name, f.getInt("seq"), p.seq)
        fun payload(b: ByteArray) = assertEquals(name, f.getString("payloadHex"), b.hex())
        when (p) {
            is Packet.Hello -> {
                assertEquals(name, 1, f.getInt("type"))
                assertEquals(name, f.getInt("version"), p.version)
                assertEquals(name, f.getInt("maxPacket"), p.maxPacket)
                assertEquals(name, f.getLong("maxMessage"), p.maxMessage)
                assertEquals(name, f.getInt("window"), p.window)
                assertEquals(name, f.getInt("flags"), p.flags)
                assertEquals(name, f.getInt("timeout"), p.timeout)
            }
            is Packet.MsgStart -> {
                assertEquals(name, 2, f.getInt("type"))
                assertEquals(name, f.getInt("msgId"), p.msgId)
                assertEquals(name, f.getLong("totalLen"), p.totalLen)
                assertEquals(name, f.getString("crc32"), "%08x".format(p.crc32))
                payload(p.payload)
            }
            is Packet.MsgData -> { assertEquals(name, 3, f.getInt("type")); assertEquals(name, f.getInt("msgId"), p.msgId); payload(p.payload) }
            is Packet.Ack -> { assertEquals(name, 4, f.getInt("type")); assertEquals(name, f.getInt("msgId"), p.msgId); assertEquals(name, f.getInt("status"), p.status) }
            is Packet.Resume -> { assertEquals(name, 5, f.getInt("type")); assertEquals(name, f.getInt("msgId"), p.msgId); assertEquals(name, f.getLong("offset"), p.offset) }
            is Packet.Cancel -> { assertEquals(name, 6, f.getInt("type")); assertEquals(name, f.getInt("msgId"), p.msgId); assertEquals(name, f.getInt("reason"), p.reason) }
            is Packet.Progress -> { assertEquals(name, 7, f.getInt("type")); assertEquals(name, f.getInt("msgId"), p.msgId); assertEquals(name, f.getLong("bytesReceived"), p.bytesReceived) }
            is Packet.MsgResend -> {
                assertEquals(name, 8, f.getInt("type")); assertEquals(name, f.getInt("msgId"), p.msgId)
                assertEquals(name, f.getLong("offset"), p.offset); payload(p.payload)
            }
            is Packet.Unknown -> throw AssertionError("$name decoded as unknown")
        }
    }

    @Test
    fun infoCharacteristic() {
        val bytes = hex(Vectors.json.getJSONObject("info").getString("hex"))
        val info = InfoCodec.decode(bytes)!!
        assertEquals(1, info.version)
        assertEquals(509, info.maxPacket)
        assertEquals(16L * 1024 * 1024, info.maxMessage)
        assertEquals(4, info.window)
        assertEquals(1, info.flags)
        assertEquals(0, info.timeout)
        assertNull(InfoCodec.decode(bytes.copyOf(9)))
    }

    @Test
    fun forwardCompatibility() {
        // unknown type keeps its seq
        assertEquals(Packet.Unknown(9, 0x42), PacketCodec.decode(byteArrayOf(0x42, 9, 1, 2, 3)))
        // shorter than the fixed layout: treated as unknown (ignored, seq counts)
        assertTrue(PacketCodec.decode(hex("04010100")) is Packet.Unknown)
        assertTrue(PacketCodec.decode(hex("0100010203")) is Packet.Unknown)
        // trailing bytes after fixed-size packets are ignored
        assertEquals(Packet.Ack(1, 1, 0), PacketCodec.decode(hex("0401010000ffee")))
        assertEquals(Packet.Hello(0, 1, 244, 16777216, 4, 1, 40), PacketCodec.decode(hex("010001f40000000001040128" + "abcd")))
        // no seq at all
        assertNull(PacketCodec.decode(byteArrayOf(1)))
    }
}
