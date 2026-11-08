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
    fun videoDetailsIsOneByte() {
        assertArrayEquals(byteArrayOf(0x0A, 1, 0, 1), Proto.videoDetails(true))
        assertArrayEquals(byteArrayOf(0x0A, 1, 0, 0), Proto.videoDetails(false))
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
            Proto.encode(Proto.VIDEO_ACK, byteArrayOf(7, 0)) +
            Proto.encode(Proto.HELLO_REPLY, ByteArray(0))
        for (chunk in listOf(1, 2, 5, bytes.size)) {
            val got = decodeAll(Proto.Decoder(), bytes, chunk)
            assertEquals(listOf(Proto.HELLO, Proto.VIDEO_ACK, Proto.HELLO_REPLY), got.map { it.type })
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
        val got = decodeAll(dec, bad + Proto.encode(Proto.VIDEO_ACK, byteArrayOf(9, 0)), 700)
        assertEquals(1, dec.oversized)
        assertEquals(listOf(Proto.VIDEO_ACK), got.map { it.type })
        assertEquals(9, Proto.u16(got[0].payload, 0))
    }

    @Test
    fun pictureIsStartPlusDataChunks() {
        val jpeg = ByteArray(5000) { (it * 7).toByte() }
        val got = decodeAll(Proto.Decoder(), Proto.picture(0x0203, 640, 360, Proto.FMT_JPEG, jpeg, 77), 333)
        assertEquals(Proto.VIDEO_START, got[0].type)
        val s = got[0].payload
        assertEquals(13, s.size)                         // 11 + phoneMs
        assertEquals(0x0203, Proto.u16(s, 0))
        assertEquals(640, Proto.u16(s, 2))
        assertEquals(360, Proto.u16(s, 4))
        assertEquals(Proto.FMT_JPEG, s[6].toInt())
        assertEquals(5000, Proto.u32(s, 7))
        assertEquals(77, Proto.u16(s, 11))
        val data = got.drop(1)
        assertEquals(3, data.size)                       // 2046 + 2046 + 908
        assert(data.all { it.type == Proto.VIDEO_DATA && Proto.u16(it.payload, 0) == 0x0203 })
        val joined = data.fold(ByteArray(0)) { acc, f -> acc + f.payload.copyOfRange(2, f.payload.size) }
        assertArrayEquals(jpeg, joined)
    }

    @Test
    fun pictureOfExactlyOneChunk() {
        val jpeg = ByteArray(Proto.MAX_PAYLOAD - 2) { 1 }
        val got = decodeAll(Proto.Decoder(), Proto.picture(1, 2, 2, Proto.FMT_JPEG, jpeg, 100000), 4096)
        assertEquals(listOf(Proto.VIDEO_START, Proto.VIDEO_DATA), got.map { it.type })
        assertEquals(Proto.MAX_PAYLOAD, got[1].payload.size)
        assertEquals(0xffff, Proto.u16(got[0].payload, 11))     // phoneMs clamped
    }

    @Test
    fun capsAndAckParse() {
        // src/audi_stream_receiver/video.c video_caps(): 800x452, 400x480, 512 KB, JPEG
        val caps = Proto.videoCapsOrNull(byteArrayOf(
            0x20, 0x03, 0xc4.toByte(), 0x01, 0x90.toByte(), 0x01, 0xe0.toByte(), 0x01,
            0, 0, 8, 0, 1))!!
        assertEquals(Proto.VideoCaps(800, 452, 400, 480, 512 * 1024, 1), caps)
        assert(caps.jpeg)
        assert(!caps.takes(Proto.FMT_H264))
        assertNull(Proto.videoCapsOrNull(ByteArray(12)))
        assertEquals(Proto.VideoAck(0x0102, Proto.VIDEO_SHOWN, 300),
            Proto.videoAckOrNull(byteArrayOf(2, 1, 0, 0x2c, 1)))
        assertNull(Proto.videoAckOrNull(ByteArray(4)))
    }

    @Test
    fun fitKeepsShapeAndOrientation() {
        val caps = Proto.VideoCaps(800, 452, 400, 480, 512 * 1024, 1)
        // Redmi Note 8 Pro, 1080x2340
        assertEquals(VideoFit.Size(220, 480), VideoFit.fit(1080, 2340, caps))
        assertEquals(VideoFit.Size(800, 368), VideoFit.fit(2340, 1080, caps))
        // 16:9 is width-bound in 800x452 too, just
        assertEquals(VideoFit.Size(800, 450), VideoFit.fit(1920, 1080, caps))
        val tablet = VideoFit.fit(1200, 1600, caps)      // 3:4 portrait
        assertEquals(VideoFit.Size(360, 480), tablet)
        val square = VideoFit.fit(1000, 1000, caps)      // square counts as landscape
        assertEquals(VideoFit.Size(452, 452), square)
    }

    @Test
    fun captureFactorLeavesUnder2To1ForAndroid() {
        // Redmi Note 8 Pro: 1600x736 and 880x1920 captures
        assertEquals(2, VideoFit.factor(2340, 1080, VideoFit.Size(800, 368)))
        assertEquals(4, VideoFit.factor(1080, 2340, VideoFit.Size(220, 480)))
        assertEquals(2, VideoFit.factor(1920, 1080, VideoFit.Size(800, 450)))
        assertEquals(1, VideoFit.factor(1280, 720, VideoFit.Size(800, 450)))  // no room for 2x
        assertEquals(VideoFit.MAX_FACTOR, VideoFit.factor(8000, 8000, VideoFit.Size(100, 100)))
    }

    @Test
    fun capsWithH264Limits() {
        // proto.h VIDEO_CAPS with H.264: 13 bytes + 720, 576, 25, 256 KB
        val p = byteArrayOf(0x20, 3, 0xe0.toByte(), 1, 0x90.toByte(), 1, 0xe0.toByte(), 1,
            0, 0, 8, 0, 7, 0xd0.toByte(), 2, 0x40, 2, 25, 0, 0, 4, 0)
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
