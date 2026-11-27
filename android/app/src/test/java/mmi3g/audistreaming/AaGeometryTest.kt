package mmi3g.audistreaming

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs

class AaGeometryTest {
    private val m800 = AaGeometry.MODES[0]
    private val m720 = AaGeometry.MODES[1]
    private val m1080 = AaGeometry.MODES[2]
    private val m1to1 = AaGeometry.MODES[3]

    @Test
    fun modesAndTheirDpi() {
        assertEquals(listOf("800x480", "1280x720", "1920x1080", "800x480 1:1"), AaGeometry.MODES.map { it.toString() })
        assertEquals(140, m800.dpi)
        assertEquals(140, m1to1.dpi)
        assertEquals(210, m720.dpi)
        assertEquals(315, m1080.dpi)
    }

    @Test
    fun fiveThreeFillsThePicture() {
        val g = AaGeometry.geometry(m800, 720, 432)
        assertEquals(ScreenCrop.Geometry(800, 480, 0, 0, 720, 432, 1, 800, 480), g)
        assertTrue(g.scaled)
        assertTrue(GlBox.linear(g))           // bilinear, not nearest
        // the kept part is the whole texture
        val q = GlBox.quadTex(g)
        assertEquals(0f, q[0], 1e-6f)
        assertEquals(0f, q[1], 1e-6f)
        assertEquals(1f, q[6], 1e-6f)
        assertEquals(1f, q[7], 1e-6f)
    }

    @Test
    fun railIsMeasuredWidth() {
        assertEquals(0, m800.rail)
        // 59 of 720 columns on the desk: 104.9 and 157.3 px
        assertEquals(105, m720.rail)
        assertEquals(158, m1080.rail)
    }

    @Test
    fun sixteenNineLeavesTheRailOutAndFills() {
        val g = AaGeometry.geometry(m720, 720, 432)
        assertEquals(ScreenCrop.Geometry(1280, 720, 105, 0, 720, 432, 1, 1175, 720), g)
        assertEquals(ScreenCrop.Geometry(1920, 1080, 158, 0, 720, 432, 1, 1762, 1080),
            AaGeometry.geometry(m1080, 720, 432))
        // u from the rail's edge to the right edge, v the whole height
        val q = GlBox.quadTex(g)
        assertEquals(105f / 1280, q[0], 1e-6f)
        assertEquals(1f, q[2], 1e-6f)
        assertEquals(0f, q[1], 1e-6f)
        assertEquals(1f, q[5], 1e-6f)
    }

    @Test
    fun oneToOneCopiesTheMapCardBesideTheRail() {
        // desk: margins 10 / 16 + ui_config at 140 dpi -> 70 px rail, map card x 70-789, y 0-463
        assertTrue(m1to1.oneToOne)
        assertTrue(m1to1.hasRail)
        assertEquals(70, m1to1.rail)
        assertEquals(720 to 464, m1to1.picW to m1to1.picH)
        val g = AaGeometry.geometry(m1to1)
        assertEquals(ScreenCrop.Geometry(800, 480, 70, 0, 720, 464, 1), g)
        assertTrue(!g.scaled)
        assertTrue(!GlBox.linear(g))          // nearest: a plain copy
        val q = GlBox.quadTex(g)
        assertEquals(70f / 800, q[0], 1e-6f)
        assertEquals(790f / 800, q[2], 1e-6f)
        // the other modes keep the scaled 720x432 picture, no margins
        for (m in AaGeometry.MODES.take(3)) {
            assertTrue(!m.oneToOne)
            assertEquals(720 to 432, m.picW to m.picH)
        }
        // a 790x480 area is 5:3 or narrower: the bar, no rail (desk)
        assertTrue(!AaGeometry.Mode(800, 480, 10, 0).hasRail)
    }

    @Test
    fun scaledTapsAverageTheFootprint() {
        // 0.9: one tap, as plain bilinear
        assertEquals(listOf(0.0 to 0.0), GlBox.scaledTaps(AaGeometry.geometry(m800, 720, 432)))
        // 1175x720 -> 720x432: 2 x 2 taps within the ~1.63 x 1.67 footprint
        val t = GlBox.scaledTaps(AaGeometry.geometry(m720, 720, 432))
        assertEquals(4, t.size)
        assertTrue(t.all { (x, y) -> abs(x) < 0.82 && abs(y) < 0.84 })
        assertEquals(0.0, t.sumOf { it.first }, 1e-9)
    }

    @Test
    fun touchScalesToTheVideoAndClamps() {
        // a 1600x960 preview of the 800x480 video: halves
        assertEquals(400 to 240, AaGeometry.touch(800f, 480f, 1600, 960, m800))
        // the preview at the MMI's 720x432: 0.9 -> 1/0.9
        assertEquals(400 to 240, AaGeometry.touch(360f, 216f, 720, 432, m800))
        // a 1080x608 preview of 1920x1080
        assertEquals(960 to 540, AaGeometry.touch(540f, 304f, 1080, 608, m1080))
        assertEquals(0 to 0, AaGeometry.touch(-5f, -5f, 1600, 960, m800))
        assertEquals(799 to 479, AaGeometry.touch(1600f, 960f, 1600, 960, m800))
        assertEquals(0 to 0, AaGeometry.touch(10f, 10f, 0, 0, m800))
    }
}
