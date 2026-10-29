package com.bluetoothserver.blelink.codec

/**
 * CRC-32/IEEE 802.3 (zlib/PNG): reflected, poly 0xEDB88320, init 0xFFFFFFFF, xorout 0xFFFFFFFF.
 * PROTOCOL.md section 2.4. Streaming: start with [INITIAL], fold data with [update], finish with [finish].
 */
object Crc32 {
    const val INITIAL: Int = -1 // 0xFFFFFFFF

    private val TABLE = IntArray(256).also { t ->
        for (n in 0 until 256) {
            var c = n
            repeat(8) { c = if (c and 1 != 0) (c ushr 1) xor 0xEDB88320.toInt() else c ushr 1 }
            t[n] = c
        }
    }

    /** Folds `data[from until from+len]` into a running (not yet finished) register value. */
    fun update(state: Int, data: ByteArray, from: Int = 0, len: Int = data.size - from): Int {
        var c = state
        for (i in from until from + len) c = TABLE[(c xor data[i].toInt()) and 0xFF] xor (c ushr 8)
        return c
    }

    fun finish(state: Int): Int = state.inv()

    /** One-shot CRC of the whole array, returned as an unsigned value in a Long. */
    fun of(data: ByteArray): Long = finish(update(INITIAL, data)).toLong() and 0xFFFFFFFFL
}
