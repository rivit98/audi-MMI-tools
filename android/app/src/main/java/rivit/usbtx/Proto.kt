package rivit.usbtx

/**
 * usb_tx_test wire protocol - must match the MMI side,
 * src/usb_tx_test/proto.h:
 *
 *   type u8 | len u16 LE | payload[len]      (len <= MAX_PAYLOAD)
 *
 *   HELLO 0x01 (both, "who I am", answered by HELLO_REPLY), TEXT 0x02
 *   (phone: seq u16 LE + UTF-8), ACK 0x03 (MMI: seq u16 LE),
 *   HELLO_REPLY 0x04 (both).
 */
object Proto {
    const val HELLO = 0x01
    const val TEXT = 0x02
    const val ACK = 0x03
    const val HELLO_REPLY = 0x04

    const val HDR = 3
    const val MAX_PAYLOAD = 2048
    const val MAX_TEXT_BYTES = MAX_PAYLOAD - 2

    fun encode(type: Int, payload: ByteArray): ByteArray {
        require(payload.size <= MAX_PAYLOAD) { "payload ${payload.size} > $MAX_PAYLOAD" }
        val out = ByteArray(HDR + payload.size)
        out[0] = type.toByte()
        out[1] = (payload.size and 0xff).toByte()
        out[2] = (payload.size shr 8).toByte()
        payload.copyInto(out, HDR)
        return out
    }

    /** TEXT frame, or null if the UTF-8 text is over MAX_TEXT_BYTES. */
    fun textOrNull(seq: Int, text: String): ByteArray? {
        val utf8 = text.toByteArray(Charsets.UTF_8)
        if (utf8.size > MAX_TEXT_BYTES) return null
        return encode(TEXT, byteArrayOf((seq and 0xff).toByte(), ((seq shr 8) and 0xff).toByte()) + utf8)
    }

    fun text(seq: Int, text: String): ByteArray =
        requireNotNull(textOrNull(seq, text)) { "text too long" }

    fun u16(p: ByteArray, off: Int): Int = (p[off].toInt() and 0xff) or ((p[off + 1].toInt() and 0xff) shl 8)

    /** Reassembles frames from arbitrary chunks; oversized frames are skipped. */
    class Decoder {
        private val buf = ByteArray(HDR + MAX_PAYLOAD)
        private var have = 0
        private var skip = 0
        var oversized = 0
            private set

        fun feed(data: ByteArray, off: Int, n: Int, onFrame: (type: Int, payload: ByteArray) -> Unit) {
            var i = off
            var left = n
            while (left > 0) {
                if (skip > 0) {
                    val take = minOf(left, skip)
                    skip -= take; i += take; left -= take
                    continue
                }
                val want = if (have < HDR) HDR - have else HDR + u16(buf, 1) - have
                val take = minOf(left, want)
                data.copyInto(buf, have, i, i + take)
                have += take; i += take; left -= take
                if (have < HDR) continue
                val len = u16(buf, 1)
                if (len > MAX_PAYLOAD) {
                    oversized++
                    skip = len
                    have = 0
                    continue
                }
                if (have == HDR + len) {
                    onFrame(buf[0].toInt() and 0xff, buf.copyOfRange(HDR, HDR + len))
                    have = 0
                }
            }
        }
    }
}
