package mmi3g.usbtx

/**
 * usb_tx_test wire protocol - must match the MMI side,
 * src/usb_tx_test/proto.h:
 *
 *   type u8 | len u16 LE | payload[len]      (len <= MAX_PAYLOAD)
 *
 *   HELLO 0x01 (both, "who I am", answered by HELLO_REPLY), HELLO_REPLY
 *   0x04 (both). 0x02 / 0x03 were TEXT / ACK, retired on 2026-10-25.
 *
 *   Video, one JPEG per picture (all integers LE):
 *   VIDEO_CAPS 0x05 (MMI: landW u16, landH u16, portW u16, portH u16,
 *   maxBytes u32, formats u8), VIDEO_START 0x06 (phone: id u16, w u16,
 *   h u16, format u8, size u32), VIDEO_DATA 0x07 (phone: id u16 + bytes),
 *   VIDEO_ACK 0x08 (MMI: id u16, status u8, decodeMs u16), VIDEO_STOP 0x09
 *   (phone, empty), VIDEO_DETAILS 0x0A (phone: show u8, 1 = the MMI's
 *   status bar over a landscape picture, 0 = the picture alone).
 */
object Proto {
    const val HELLO = 0x01
    const val HELLO_REPLY = 0x04
    const val VIDEO_CAPS = 0x05
    const val VIDEO_START = 0x06
    const val VIDEO_DATA = 0x07
    const val VIDEO_ACK = 0x08
    const val VIDEO_STOP = 0x09
    const val VIDEO_DETAILS = 0x0A

    const val FMT_JPEG = 1

    // VIDEO_ACK status
    const val VIDEO_SHOWN = 0
    const val VIDEO_BAD = 1
    const val VIDEO_DROPPED = 2

    const val HDR = 3
    const val MAX_PAYLOAD = 2048

    fun encode(type: Int, payload: ByteArray): ByteArray {
        require(payload.size <= MAX_PAYLOAD) { "payload ${payload.size} > $MAX_PAYLOAD" }
        val out = ByteArray(HDR + payload.size)
        out[0] = type.toByte()
        out[1] = (payload.size and 0xff).toByte()
        out[2] = (payload.size shr 8).toByte()
        payload.copyInto(out, HDR)
        return out
    }

    /** The boxes a landscape / portrait picture must fit in (VIDEO_CAPS). */
    data class VideoCaps(val landW: Int, val landH: Int, val portW: Int, val portH: Int,
                         val maxBytes: Int, val formats: Int) {
        val jpeg: Boolean get() = formats and (1 shl (FMT_JPEG - 1)) != 0
    }

    data class VideoAck(val id: Int, val status: Int, val decodeMs: Int)

    fun videoCapsOrNull(p: ByteArray): VideoCaps? =
        if (p.size < 13) null
        else VideoCaps(u16(p, 0), u16(p, 2), u16(p, 4), u16(p, 6), u32(p, 8), p[12].toInt() and 0xff)

    fun videoAckOrNull(p: ByteArray): VideoAck? =
        if (p.size < 5) null else VideoAck(u16(p, 0), p[2].toInt() and 0xff, u16(p, 3))

    fun videoDetails(show: Boolean): ByteArray =
        encode(VIDEO_DETAILS, byteArrayOf(if (show) 1 else 0))

    /** One picture as VIDEO_START + VIDEO_DATA frames, back to back. */
    fun picture(id: Int, w: Int, h: Int, jpeg: ByteArray): ByteArray {
        val chunk = MAX_PAYLOAD - 2
        val nData = (jpeg.size + chunk - 1) / chunk
        val out = ByteArray(HDR + 11 + nData * (HDR + 2) + jpeg.size)
        val start = ByteArray(11)
        put16(start, 0, id); put16(start, 2, w); put16(start, 4, h)
        start[6] = FMT_JPEG.toByte()
        put32(start, 7, jpeg.size)
        encode(VIDEO_START, start).copyInto(out, 0)
        var o = HDR + start.size
        var i = 0
        while (i < jpeg.size) {
            val n = minOf(chunk, jpeg.size - i)
            out[o] = VIDEO_DATA.toByte()
            out[o + 1] = ((n + 2) and 0xff).toByte()
            out[o + 2] = ((n + 2) shr 8).toByte()
            put16(out, o + 3, id)
            jpeg.copyInto(out, o + 5, i, i + n)
            o += HDR + 2 + n
            i += n
        }
        return out
    }

    private fun put16(p: ByteArray, off: Int, v: Int) {
        p[off] = (v and 0xff).toByte()
        p[off + 1] = ((v shr 8) and 0xff).toByte()
    }

    private fun put32(p: ByteArray, off: Int, v: Int) {
        put16(p, off, v and 0xffff)
        put16(p, off + 2, (v ushr 16) and 0xffff)
    }

    fun u32(p: ByteArray, off: Int): Int = u16(p, off) or (u16(p, off + 2) shl 16)

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
