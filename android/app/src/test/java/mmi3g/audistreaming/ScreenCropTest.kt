package mmi3g.audistreaming

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ScreenCropTest {
    /** The H.264 path's numbers for a screen and insets. */
    private fun h264(w: Int, h: Int, i: ScreenCrop.Insets): ScreenCrop.Geometry {
        val cw = i.contentW(w)
        val ch = i.contentH(h)
        val pic = VideoFit.fit(cw, ch, 720, 432)
        return ScreenCrop.geometry(w, h, i, pic.w, pic.h, GlBox.factor(cw, ch, 720, 432))
    }

    @Test
    fun landscapeBarOnTheRightGivesATallerPicture() {
        val g = h264(2340, 1080, ScreenCrop.Insets(right = 132))
        assertEquals(720, g.picW)
        assertEquals(352, g.picH)
        assertEquals(3, g.k)
        assertEquals(2289, g.capW)
        assertEquals(1057, g.capH)
        assertEquals(0, g.cropX)
        assertEquals(0, g.cropY)
    }

    @Test
    fun landscapeBarOnTheLeftKeepsTheRightOfTheCapture() {
        val g = h264(2340, 1080, ScreenCrop.Insets(left = 132))
        assertEquals(2289 - 2160, g.cropX)
        assertEquals(2289, g.cropX + g.picW * g.k)
    }

    @Test
    fun portraitBarAtTheBottom() {
        val g = h264(1080, 2340, ScreenCrop.Insets(bottom = 132))
        assertEquals(210, g.picW)
        assertEquals(432, g.picH)
        assertEquals(4, g.k)
        assertEquals(0, g.cropY)
        // what stays of the screen's height is the kept part, the bar is under it
        assertEquals(1832.0, g.capH.toDouble(), 1.0)
        assertTrue(g.capH >= g.picH * g.k)
    }

    @Test
    fun noInsetsIsTheOldLetterboxedContent() {
        val g = h264(2340, 1080, ScreenCrop.NONE)
        assertEquals(720, g.picW)
        assertEquals(332, g.picH)
        assertEquals(3, g.k)
        assertEquals(2160, g.capW)
    }

    @Test
    fun keptPartStaysInsideTheCapture() {
        for (w in listOf(1080, 1440, 1920, 2340, 2400, 3200)) for (h in listOf(720, 1080, 1440)) {
            for (i in listOf(ScreenCrop.NONE, ScreenCrop.Insets(right = 132), ScreenCrop.Insets(bottom = 96),
                ScreenCrop.Insets(left = 77))) {
                for (g in listOf(h264(w, h, i), h264(h, w, i))) {
                    assertTrue("$w x $h $i $g", g.cropX >= 0 && g.cropX + g.picW * g.k <= g.capW)
                    assertTrue("$w x $h $i $g", g.cropY >= 0 && g.cropY + g.picH * g.k <= g.capH)
                    assertTrue("$w x $h $i $g", g.picW <= 720 && g.picH <= 432)
                }
            }
        }
    }

    @Test
    fun insetsThatCanNotBeABarAreIgnored() {
        val bar = ScreenCrop.Insets(right = 132)
        assertEquals(bar, ScreenCrop.sane(bar, 2340, 1080))
        assertEquals(ScreenCrop.NONE, ScreenCrop.sane(ScreenCrop.Insets(right = 500), 2340, 1080))
        assertEquals(ScreenCrop.NONE, ScreenCrop.sane(ScreenCrop.Insets(bottom = 300), 2340, 1080))
        assertEquals(ScreenCrop.NONE, ScreenCrop.sane(ScreenCrop.Insets(left = -1), 2340, 1080))
    }

    @Test
    fun quadTexturePicksTheKeptPartBottomFirst() {
        // 100x200 capture, kept 60x120 from (10, 20): x 0.1..0.7, y from the
        // top 0.1..0.7 = from the bottom 0.3..0.9
        val g = ScreenCrop.Geometry(100, 200, 10, 20, 30, 60, 2)
        assertArrayEquals(floatArrayOf(0.1f, 0.3f, 0.7f, 0.3f, 0.1f, 0.9f, 0.7f, 0.9f), GlBox.quadTex(g), 1e-6f)
    }

    @Test
    fun boxReadsFromTheOffset() {
        // a 4x2 capture, 2x2 kept from (1, 0), k = 2: only the middle counts
        val src = intArrayOf(0x111111, 0x101010, 0x202020, 0x777777, 0x222222, 0x101010, 0x202020, 0x777777)
        val out = IntArray(1)
        Downscale.box(src, 4, 2, 1, 1, out, 1)
        assertEquals(0xff181818.toInt(), out[0])
    }
}
