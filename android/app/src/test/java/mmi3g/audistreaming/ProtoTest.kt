package mmi3g.audistreaming

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Test

class ProtoTest {
    private data class Frame(val type: Int, val payload: ByteArray)

    private fun decodeAll(dec: Proto.Decoder, bytes: ByteArray, chunk: Int): List<Frame> {
        val out = mutableListOf<Frame>()
        var i = 0
        while (i < bytes.size) {
            val n = minOf(chunk, bytes.size - i)
            dec.feed(bytes, i, n) { t, p -> out += Frame(t, p) }
            i += n
        }
        return out
    }

    @Test
    fun encodeMatchesTheCLayout() {
        // type u8 | len u16 LE | payload  (src/audi_stream_receiver/proto.h)
        assertArrayEquals(
            byteArrayOf(7, 4, 0, 5, 0, 'h'.code.toByte(), 'i'.code.toByte()),
            Proto.encode(Proto.VIDEO_DATA, byteArrayOf(5, 0, 'h'.code.toByte(), 'i'.code.toByte())),
        )
        assertArrayEquals(byteArrayOf(9, 0, 0), Proto.encode(Proto.VIDEO_STOP, ByteArray(0)))
    }

    @Test
    fun streamSettingsIsPrintableAscii() {
        val f = Proto.streamSettings("format=H264 fps=25 é")
        assertArrayEquals(byteArrayOf(0x0E, 20, 0), f.copyOfRange(0, 3))
        assertEquals("format=H264 fps=25 ?", String(f, 3, f.size - 3, Charsets.US_ASCII))
        assertEquals(3 + Proto.MAX_PAYLOAD, Proto.streamSettings("x".repeat(5000)).size)
    }

    @Test
    fun helloCarriesUtf8() {
        val f = Proto.encode(Proto.HELLO, "zażółć".toByteArray())
        val got = decodeAll(Proto.Decoder(), f, 1)
        assertEquals(1, got.size)
        assertEquals(Proto.HELLO, got[0].type)
        assertEquals("zażółć", String(got[0].payload, Charsets.UTF_8))
    }

    @Test
    fun payloadOverMaxIsRejected() {
        assertEquals(Proto.HDR + Proto.MAX_PAYLOAD, Proto.encode(Proto.HELLO, ByteArray(Proto.MAX_PAYLOAD)).size)
        assertThrows(IllegalArgumentException::class.java) {
            Proto.encode(Proto.HELLO, ByteArray(Proto.MAX_PAYLOAD + 1))
        }
    }

    @Test
    fun mergedAndSplitFrames() {
        val bytes = Proto.encode(Proto.HELLO, "mmi".toByteArray()) +
            Proto.encode(Proto.VIDEO_ACKS, byteArrayOf(7, 0)) +
            Proto.encode(Proto.HELLO_REPLY, ByteArray(0))
        for (chunk in listOf(1, 2, 5, bytes.size)) {
            val got = decodeAll(Proto.Decoder(), bytes, chunk)
            assertEquals(listOf(Proto.HELLO, Proto.VIDEO_ACKS, Proto.HELLO_REPLY), got.map { it.type })
            assertEquals("mmi", String(got[0].payload))
            assertEquals(7, Proto.u16(got[1].payload, 0))
            assertEquals(0, got[2].payload.size)
        }
    }

    @Test
    fun oversizedFrameIsSkipped() {
        val big = Proto.MAX_PAYLOAD + 10
        val bad = byteArrayOf(Proto.VIDEO_DATA.toByte(), (big and 0xff).toByte(), (big shr 8).toByte()) +
            ByteArray(big) { 0x55 }
        val dec = Proto.Decoder()
        val got = decodeAll(dec, bad + Proto.encode(Proto.VIDEO_ACKS, byteArrayOf(9, 0)), 700)
        assertEquals(1, dec.oversized)
        assertEquals(listOf(Proto.VIDEO_ACKS), got.map { it.type })
        assertEquals(9, Proto.u16(got[0].payload, 0))
    }

    @Test
    fun h264FrameIsStartPlusDataChunks() {
        val frame = ByteArray(5000) { (it * 7).toByte() }
        val got = decodeAll(Proto.Decoder(), Proto.h264Frame(0x0203, 720, 432, frame), 333)
        assertEquals(Proto.VIDEO_START, got[0].type)
        val s = got[0].payload
        assertEquals(13, s.size)                         // 11 + phoneMs
        assertEquals(0x0203, Proto.u16(s, 0))
        assertEquals(720, Proto.u16(s, 2))
        assertEquals(432, Proto.u16(s, 4))
        assertEquals(Proto.FMT_H264, s[6].toInt())
        assertEquals(3, Proto.FMT_H264)
        assertEquals(5000, Proto.u32(s, 7))
        assertEquals(0, Proto.u16(s, 11))
        val data = got.drop(1)
        assertEquals(3, data.size)                       // 2046 + 2046 + 908
        assert(data.all { it.type == Proto.VIDEO_DATA && Proto.u16(it.payload, 0) == 0x0203 })
        val joined = data.fold(ByteArray(0)) { acc, f -> acc + f.payload.copyOfRange(2, f.payload.size) }
        assertArrayEquals(frame, joined)
    }

    @Test
    fun h264FrameOfExactlyOneChunk() {
        val frame = ByteArray(Proto.MAX_PAYLOAD - 2) { 1 }
        val got = decodeAll(Proto.Decoder(), Proto.h264Frame(1, 2, 2, frame), 4096)
        assertEquals(listOf(Proto.VIDEO_START, Proto.VIDEO_DATA), got.map { it.type })
        assertEquals(Proto.MAX_PAYLOAD, got[1].payload.size)
    }

    @Test
    fun capsAndAckParse() {
        // an old 13-byte VIDEO_CAPS: 800x452, 400x480, 512 KB, only the
        // retired JPEG bit
        val caps = Proto.videoCapsOrNull(byteArrayOf(
            0x20, 0x03, 0xc4.toByte(), 0x01, 0x90.toByte(), 0x01, 0xe0.toByte(), 0x01,
            0, 0, 8, 0, 1))!!
        assertEquals(Proto.VideoCaps(800, 452, 400, 480, 512 * 1024, 1), caps)
        assert(!caps.takes(Proto.FMT_H264))
        assert(!caps.h264)
        assertNull(Proto.videoCapsOrNull(ByteArray(12)))
        // proto.h VIDEO_ACKS: shown u16, bad u16, dropped u16
        val acks = Proto.videoAcksOrNull(byteArrayOf(2, 1, 3, 0, 5, 4))!!
        assertEquals(Proto.VideoAcks(0x0102, 3, 0x0405), acks)
        assertEquals(0x0102 + 3 + 0x0405, acks.total)
        assertNull(Proto.videoAcksOrNull(ByteArray(5)))
        assertEquals(0x0D, Proto.VIDEO_ACKS)
    }

    @Test
    fun fitKeepsShape() {
        // Redmi Note 8 Pro, 2340x1080, into the 720x432 H.264 picture
        assertEquals(VideoFit.Size(720, 332), VideoFit.fit(2340, 1080, 720, 432))
        assertEquals(VideoFit.Size(198, 432), VideoFit.fit(1080, 2340, 720, 432))
        // 1920 * (800 / 1920.0) must not come out as 799 (798)
        assertEquals(VideoFit.Size(800, 450), VideoFit.fit(1920, 1080, 800, 452))
        assertEquals(VideoFit.Size(432, 432), VideoFit.fit(1000, 1000, 720, 432))
        assert(VideoFit.portrait(1080, 2340))
        assert(!VideoFit.portrait(1000, 1000))        // square counts as landscape
    }

    @Test
    fun capsWithH264Limits() {
        // proto.h VIDEO_CAPS with H.264: 13 bytes + 720, 576, 25, 256 KB
        val p = byteArrayOf(0x20, 3, 0xe0.toByte(), 1, 0x90.toByte(), 1, 0xe0.toByte(), 1,
            0, 0, 8, 0, 4, 0xd0.toByte(), 2, 0x40, 2, 25, 0, 0, 4, 0)
        val caps = Proto.videoCapsOrNull(p)!!
        assertEquals(800, caps.landW)
        assertEquals(720, caps.h264MaxW)
        assertEquals(576, caps.h264MaxH)
        assertEquals(25, caps.h264Fps)
        assertEquals(256 * 1024, caps.h264MaxFrame)
        assertEquals(true, caps.h264)
        assertEquals(false, Proto.videoCapsOrNull(p.copyOf(13))!!.h264)       // older MMI app
    }

    @Test
    fun h264ConfigMatchesTheCLayout() {
        // proto.h H264_CONFIG: fps u8, frame_bytes u32, sps_len u16, sps, pps_len u16, pps
        val f = Proto.h264Config(25, 0x10000, byteArrayOf(0x67, 0x42), byteArrayOf(0x68))
        assertArrayEquals(byteArrayOf(0x0B, 12, 0, 25, 0, 0, 1, 0, 2, 0, 0x67, 0x42, 1, 0, 0x68), f)
        assertEquals(Proto.H264_SYNC, Proto.h264StatusOrNull(byteArrayOf(1)))
        assertNull(Proto.h264StatusOrNull(ByteArray(0)))
    }
}
