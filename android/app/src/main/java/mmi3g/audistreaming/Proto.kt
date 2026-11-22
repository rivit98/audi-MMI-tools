package mmi3g.audistreaming

/**
 * audi_stream_receiver wire protocol - must match the MMI side,
 * src/audi_stream_receiver/proto.h:
 *
 *   type u8 | len u16 LE | payload[len]      (len <= MAX_PAYLOAD)
 *
 *   HELLO 0x01 (both, "who I am", answered by HELLO_REPLY), HELLO_REPLY
 *   0x04 (both). 0x02 / 0x03 were TEXT / ACK, retired.
 *
 *   Video is hardware H.264 only (all integers LE; proto.h has the details):
 *   VIDEO_CAPS 0x05 (MMI: landW u16, landH u16, portW u16, portH u16,
 *   maxBytes u32, formats u8: bit n-1 = format n, h264MaxW u16, h264MaxH
 *   u16, h264Fps u8, h264MaxFrame u32; 22 bytes). Only format 3 (H.264,
 *   bit 2) is left: 1 was JPEG and 2 PNG, both retired and not to be reused.
 *   The boxes and maxBytes are still on the wire but unused here.
 *   H264_CONFIG 0x0B (phone: fps u8, frameBytes u32, spsLen u16, sps, ppsLen
 *   u16, pps; NAL units without start codes) starts or restarts the stream.
 *   Then each encoded frame (Annex-B) goes as VIDEO_START 0x06 (phone: id
 *   u16, w u16, h u16, format u8 = FMT_H264, size u32, phoneMs u16 = 0) +
 *   VIDEO_DATA 0x07 (phone: id u16 + bytes). VIDEO_ACKS 0x0D (MMI: shown
 *   u16, bad u16, dropped u16: this many more frames are done, counted on
 *   arrival, in batches). VIDEO_STOP 0x09 (phone, empty). H264_STATUS 0x0C
 *   (MMI: code u8): PLAYING, SYNC (send an IDR), REFUSED (pause H.264 on
 *   this link and try again later, [RefusedBackoff]), CONFIG (send
 *   H264_CONFIG again). 0x08 was VIDEO_ACK, one per picture, retired; 0x0A
 *   was VIDEO_DETAILS (a status bar over a landscape picture), retired.
 *
 *   STREAM_SETTINGS 0x0E (phone: ASCII "key=value ...", the settings
 *   streaming runs with, for the MMI's log only; older MMI apps log it as
 *   unknown and ignore it).
 */
object Proto {
    const val HELLO = 0x01
    const val HELLO_REPLY = 0x04
    const val VIDEO_CAPS = 0x05
    const val VIDEO_START = 0x06
    const val VIDEO_DATA = 0x07
    const val VIDEO_STOP = 0x09
    const val H264_CONFIG = 0x0B
    const val H264_STATUS = 0x0C
    const val VIDEO_ACKS = 0x0D
    const val STREAM_SETTINGS = 0x0E

    /** VIDEO_START's format; 1 (JPEG) and 2 (PNG) are retired. */
    const val FMT_H264 = 3

    // H264_STATUS code
    const val H264_PLAYING = 0
    const val H264_SYNC = 1
    const val H264_REFUSED = 2
    const val H264_CONFIG_WANTED = 3

    const val HDR = 3
    const val START_LEN = 13
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

    /** VIDEO_CAPS: the limits of the MMI's hardware H.264 stream (0 from an
     * older MMI app). The boxes and maxBytes are still on the wire, unused. */
    data class VideoCaps(val landW: Int, val landH: Int, val portW: Int, val portH: Int,
                         val maxBytes: Int, val formats: Int,
                         val h264MaxW: Int = 0, val h264MaxH: Int = 0, val h264Fps: Int = 0,
                         val h264MaxFrame: Int = 0) {
        val h264: Boolean get() = takes(FMT_H264) && h264MaxW > 0 && h264MaxH > 0 && h264Fps > 0
        fun takes(format: Int): Boolean = formats and (1 shl (format - 1)) != 0
    }

    /** How many more pictures are done, since the last VIDEO_ACKS. */
    data class VideoAcks(val shown: Int, val bad: Int, val dropped: Int) {
        val total: Int get() = shown + bad + dropped
    }

    fun videoCapsOrNull(p: ByteArray): VideoCaps? = when {
        p.size < 13 -> null
        p.size < 22 -> VideoCaps(u16(p, 0), u16(p, 2), u16(p, 4), u16(p, 6), u32(p, 8), p[12].toInt() and 0xff)
        else -> VideoCaps(u16(p, 0), u16(p, 2), u16(p, 4), u16(p, 6), u32(p, 8), p[12].toInt() and 0xff,
            u16(p, 13), u16(p, 15), p[17].toInt() and 0xff, u32(p, 18))
    }

    /** H264_CONFIG: the stream's rate, the largest frame, SPS and PPS (NAL
     * units, no start codes). An older app added VCAP 0's field mode after the
     * PPS; the MMI ignores it and always weaves. */
    fun h264Config(fps: Int, frameBytes: Int, sps: ByteArray, pps: ByteArray): ByteArray {
        val p = ByteArray(1 + 4 + 2 + sps.size + 2 + pps.size)
        p[0] = fps.toByte()
        put32(p, 1, frameBytes)
        put16(p, 5, sps.size)
        sps.copyInto(p, 7)
        put16(p, 7 + sps.size, pps.size)
        pps.copyInto(p, 9 + sps.size)
        return encode(H264_CONFIG, p)
    }

    fun h264StatusOrNull(p: ByteArray): Int? = if (p.isEmpty()) null else p[0].toInt() and 0xff

    fun videoAcksOrNull(p: ByteArray): VideoAcks? =
        if (p.size < 6) null else VideoAcks(u16(p, 0), u16(p, 2), u16(p, 4))

    /** STREAM_SETTINGS: [text] as ASCII (anything else as '?'), cut at MAX_PAYLOAD. */
    fun streamSettings(text: String): ByteArray {
        val b = ByteArray(minOf(text.length, MAX_PAYLOAD)) { i ->
            text[i].code.let { if (it in 0x20..0x7e) it.toByte() else '?'.code.toByte() }
        }
        return encode(STREAM_SETTINGS, b)
    }

    /** One H.264 frame (Annex-B) as VIDEO_START + VIDEO_DATA frames, back
     * to back. */
    fun h264Frame(id: Int, w: Int, h: Int, data: ByteArray): ByteArray {
        val chunk = MAX_PAYLOAD - 2
        val nData = (data.size + chunk - 1) / chunk
        val out = ByteArray(HDR + START_LEN + nData * (HDR + 2) + data.size)
        val start = ByteArray(START_LEN)
        put16(start, 0, id); put16(start, 2, w); put16(start, 4, h)
        start[6] = FMT_H264.toByte()
        put32(start, 7, data.size)
        // start[11..12]: phoneMs, 0 (the encoder's time isn't known per frame)
        encode(VIDEO_START, start).copyInto(out, 0)
        var o = HDR + start.size
        var i = 0
        while (i < data.size) {
            val n = minOf(chunk, data.size - i)
            out[o] = VIDEO_DATA.toByte()
            out[o + 1] = ((n + 2) and 0xff).toByte()
            out[o + 2] = ((n + 2) shr 8).toByte()
            put16(out, o + 3, id)
            data.copyInto(out, o + 5, i, i + n)
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
