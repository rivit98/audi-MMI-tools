package mmi3g.audistreaming

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class H264Test {
    private val sc4 = byteArrayOf(0, 0, 0, 1)
    private val sc3 = byteArrayOf(0, 0, 1)
    private val sps = byteArrayOf(0x67, 0x42, 0xc0.toByte(), 0x1e, 0x11)
    private val pps = byteArrayOf(0x68, 0xce.toByte(), 0x3c, 0x80.toByte())

    @Test
    fun spsPpsFromCodecConfig() {
        val csd = sc4 + sps + sc4 + pps
        val got = H264.spsPps(csd)!!
        assertArrayEquals(sps, got.first)
        assertArrayEquals(pps, got.second)
        assertNull(H264.spsPps(sc4 + sps))
    }

    @Test
    fun nalUnitsAndSampleBytes() {
        val aud = byteArrayOf(0x09, 0xf0.toByte())
        val idr = byteArrayOf(0x65, 1, 2, 3, 0, 0)          // trailing zeros dropped
        val frame = sc4 + aud + sc3 + sps + sc3 + idr
        val nals = H264.nalUnits(frame)
        assertEquals(3, nals.size)
        assertEquals(4, nals[2].second)
        // AUD dropped; 4-byte length per NAL, as the MMI's make_sample
        assertEquals(4 + sps.size + 4 + 4, H264.sampleBytes(frame))
        assertEquals(true, H264.isIdr(frame))
        assertEquals(false, H264.isIdr(sc4 + byteArrayOf(0x41, 1, 2)))
    }

    @Test
    fun growsInStepsUpToTheMax() {
        assertEquals(128 * 1024, H264.grow(64 * 1024, 70_000, 256 * 1024))      // twice the old
        assertEquals(256 * 1024, H264.grow(64 * 1024, 200_000, 256 * 1024))     // 1.5 x need, capped
        assertEquals(256 * 1024, H264.grow(256 * 1024, 300_000, 256 * 1024))    // can't grow
    }

    @Test
    fun pictureSizeForTheMmi() {
        assertEquals(720 to 432, H264.pictureSize(720, 576))
        assertEquals(640 to 384, H264.pictureSize(640, 480))
        assertEquals(400 to 240, H264.pictureSize(1280, 240))                   // height-bound
    }
}
