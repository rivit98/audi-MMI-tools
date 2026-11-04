package mmi3g.audistreaming

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class DownscaleTest {
    private fun px(b3: Int, b2: Int, b1: Int, b0: Int) = (b3 shl 24) or (b2 shl 16) or (b1 shl 8) or b0

    @Test
    fun averagesEachByteOfABlock() {
        // 4x2 source (stride 5: one padding pixel), 2:1 -> 2x1
        val src = intArrayOf(
            px(0, 0, 0, 0), px(0, 100, 200, 255), px(9, 10, 20, 30), px(9, 10, 20, 30), -1,
            px(0, 0, 0, 0), px(0, 100, 200, 255), px(9, 10, 20, 30), px(9, 10, 20, 31), -1,
        )
        val out = IntArray(2)
        Downscale.box(src, 5, 2, 2, 1, out)
        // (0+100+0+100)/4 = 50, 400/4 = 100, 510/4 = 127.5 -> 128 (rounded); top byte 0xff
        assertEquals(px(0xff, 50, 100, 128), out[0])
        // 30,30,30,31 -> 30.25 -> 30
        assertEquals(px(0xff, 10, 20, 30), out[1])
    }

    @Test
    fun whiteStaysWhiteAtTheLargestFactor() {
        val k = Downscale.MAX_FACTOR
        val src = IntArray(k * k) { -1 }
        val out = IntArray(1)
        Downscale.box(src, k, k, 1, 1, out)
        assertEquals(-1, out[0])                     // no lane overflow into its neighbour
    }

    @Test
    fun factorOneDropsThePadding() {
        val src = intArrayOf(1, 2, 99, 3, 4, 99)
        val out = IntArray(4)
        Downscale.box(src, 3, 1, 2, 2, out)
        assertEquals(listOf(1, 2, 3, 4).map { it or (0xff shl 24) }, out.toList())
    }

    @Test
    fun oneThinLineSurvivesAsGrey() {
        // a 1-pixel black stroke in white, 4:1: one grey output pixel per
        // block it crosses, never dropped (bilinear at 4:1 could miss it)
        val k = 4
        val w = 8
        val src = IntArray(w * k) { i -> if (i % w == 5) px(0xff, 0, 0, 0) else -1 }
        val out = IntArray(2)
        Downscale.box(src, w, k, 2, 1, out)
        assertEquals(-1, out[0])
        assertEquals(px(0xff, 191, 191, 191), out[1])    // 3/4 white: 765/4 = 191.25
    }

    @Test
    fun refusesASourceTooSmall() {
        assertThrows(IllegalArgumentException::class.java) {
            Downscale.box(IntArray(3), 2, 2, 1, 1, IntArray(1))
        }
    }
}
