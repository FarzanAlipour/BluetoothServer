package com.bluetoothserver.blelink.codec

/**
 * BleLink v1 packets (PROTOCOL.md section 2.3). Pure Kotlin, no Android dependencies.
 *
 * Unsigned wire fields are widened: u8/u16 -> Int, u32 -> Long (0..0xFFFFFFFF).
 * `seq` is the per-direction packet counter (u8).
 */
sealed class Packet {
    abstract val seq: Int

    data class Hello(
        override val seq: Int,
        val version: Int,
        val maxPacket: Int,
        val maxMessage: Long,
        val window: Int,
        val flags: Int,
        val timeout: Int,
    ) : Packet()

    class MsgStart(override val seq: Int, val msgId: Int, val totalLen: Long, val crc32: Long, val payload: ByteArray) : Packet()
    class MsgData(override val seq: Int, val msgId: Int, val payload: ByteArray) : Packet()
    data class Ack(override val seq: Int, val msgId: Int, val status: Int) : Packet()
    data class Resume(override val seq: Int, val msgId: Int, val offset: Long) : Packet()
    data class Cancel(override val seq: Int, val msgId: Int, val reason: Int) : Packet()
    data class Progress(override val seq: Int, val msgId: Int, val bytesReceived: Long) : Packet()
    class MsgResend(override val seq: Int, val msgId: Int, val offset: Long, val payload: ByteArray) : Packet()

    /** Unknown type (forward compatibility, section 2.3): ignored, but its seq still counts. */
    data class Unknown(override val seq: Int, val type: Int) : Packet()
}

object PacketType {
    const val HELLO = 0x01
    const val MSG_START = 0x02
    const val MSG_DATA = 0x03
    const val ACK = 0x04
    const val RESUME = 0x05
    const val CANCEL = 0x06
    const val PROGRESS = 0x07
    const val MSG_RESEND = 0x08

    const val HELLO_SIZE = 12
    const val START_HEADER = 12
    const val DATA_HEADER = 4
    const val RESEND_HEADER = 8

    const val FLAG_PROGRESS = 0x01
    const val FLAG_STREAMING = 0x02
}

object AckStatus {
    const val OK = 0
    const val CRC = 1
    const val TOO_LARGE = 2
    const val BUSY = 3
    const val REJECTED = 4
}

object CancelReason {
    const val APPLICATION = 0
    const val GAVE_UP = 1
    const val SHUTDOWN = 2
}

/**
 * INFO characteristic value (PROTOCOL.md section 1): the HELLO body without type and seq, 10 bytes.
 * Returned as a [Packet.Hello] with seq 0. Null when shorter than 10 bytes; trailing bytes ignored.
 */
object InfoCodec {
    fun decode(b: ByteArray): Packet.Hello? {
        if (b.size < 10) return null
        val p = PacketCodec.decode(byteArrayOf(PacketType.HELLO.toByte(), 0) + b.copyOf(10))
        return p as? Packet.Hello
    }
}

/** Little-endian encoder/decoder for [Packet]. */
object PacketCodec {

    fun encode(p: Packet): ByteArray = when (p) {
        is Packet.Hello -> Writer(PacketType.HELLO_SIZE).u8(PacketType.HELLO).u8(p.seq).u8(p.version)
            .u16(p.maxPacket).u32(p.maxMessage).u8(p.window).u8(p.flags).u8(p.timeout).bytes
        is Packet.MsgStart -> Writer(PacketType.START_HEADER + p.payload.size).u8(PacketType.MSG_START).u8(p.seq)
            .u16(p.msgId).u32(p.totalLen).u32(p.crc32).raw(p.payload).bytes
        is Packet.MsgData -> Writer(PacketType.DATA_HEADER + p.payload.size).u8(PacketType.MSG_DATA).u8(p.seq)
            .u16(p.msgId).raw(p.payload).bytes
        is Packet.Ack -> Writer(5).u8(PacketType.ACK).u8(p.seq).u16(p.msgId).u8(p.status).bytes
        is Packet.Resume -> Writer(8).u8(PacketType.RESUME).u8(p.seq).u16(p.msgId).u32(p.offset).bytes
        is Packet.Cancel -> Writer(5).u8(PacketType.CANCEL).u8(p.seq).u16(p.msgId).u8(p.reason).bytes
        is Packet.Progress -> Writer(8).u8(PacketType.PROGRESS).u8(p.seq).u16(p.msgId).u32(p.bytesReceived).bytes
        is Packet.MsgResend -> Writer(PacketType.RESEND_HEADER + p.payload.size).u8(PacketType.MSG_RESEND).u8(p.seq)
            .u16(p.msgId).u32(p.offset).raw(p.payload).bytes
        is Packet.Unknown -> byteArrayOf(p.type.toByte(), p.seq.toByte())
    }

    /**
     * Decodes one packet. Returns null when the packet is shorter than 2 bytes (no seq) ;
     * returns [Packet.Unknown] for unknown types AND for known types shorter than their fixed layout
     * (section 2.3: such packets are ignored but their seq still counts). Trailing bytes after a
     * fixed-size packet are ignored.
     */
    fun decode(b: ByteArray): Packet? {
        if (b.size < 2) return null
        val type = b[0].toInt() and 0xFF
        val seq = b[1].toInt() and 0xFF
        val r = Reader(b)
        fun short(min: Int) = b.size < min
        return when (type) {
            PacketType.HELLO -> if (short(12)) Packet.Unknown(seq, type) else
                Packet.Hello(seq, r.u8(2), r.u16(3), r.u32(5), r.u8(9), r.u8(10), r.u8(11))
            PacketType.MSG_START -> if (short(12)) Packet.Unknown(seq, type) else
                Packet.MsgStart(seq, r.u16(2), r.u32(4), r.u32(8), b.copyOfRange(12, b.size))
            PacketType.MSG_DATA -> if (short(4)) Packet.Unknown(seq, type) else
                Packet.MsgData(seq, r.u16(2), b.copyOfRange(4, b.size))
            PacketType.ACK -> if (short(5)) Packet.Unknown(seq, type) else Packet.Ack(seq, r.u16(2), r.u8(4))
            PacketType.RESUME -> if (short(8)) Packet.Unknown(seq, type) else Packet.Resume(seq, r.u16(2), r.u32(4))
            PacketType.CANCEL -> if (short(5)) Packet.Unknown(seq, type) else Packet.Cancel(seq, r.u16(2), r.u8(4))
            PacketType.PROGRESS -> if (short(8)) Packet.Unknown(seq, type) else Packet.Progress(seq, r.u16(2), r.u32(4))
            PacketType.MSG_RESEND -> if (short(8)) Packet.Unknown(seq, type) else
                Packet.MsgResend(seq, r.u16(2), r.u32(4), b.copyOfRange(8, b.size))
            else -> Packet.Unknown(seq, type)
        }
    }

    /** Payload capacity of each data-carrying type at a given packet size (section 2.3). */
    fun startCapacity(packetSize: Int) = packetSize - PacketType.START_HEADER
    fun dataCapacity(packetSize: Int) = packetSize - PacketType.DATA_HEADER
    fun resendCapacity(packetSize: Int) = packetSize - PacketType.RESEND_HEADER

    private class Writer(size: Int) {
        val bytes = ByteArray(size)
        private var i = 0
        fun u8(v: Int) = apply { bytes[i++] = v.toByte() }
        fun u16(v: Int) = apply { u8(v); u8(v ushr 8) }
        fun u32(v: Long) = apply { u16((v and 0xFFFF).toInt()); u16(((v ushr 16) and 0xFFFF).toInt()) }
        fun raw(p: ByteArray) = apply { p.copyInto(bytes, i); i += p.size }
    }

    private class Reader(val b: ByteArray) {
        fun u8(o: Int) = b[o].toInt() and 0xFF
        fun u16(o: Int) = u8(o) or (u8(o + 1) shl 8)
        fun u32(o: Int) = (u16(o).toLong()) or (u16(o + 2).toLong() shl 16)
    }
}
