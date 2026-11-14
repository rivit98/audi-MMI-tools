package mmi3g.audistreaming

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class AaGeometryTest {
    private val m800 = AaGeometry.MODES[0]
    private val m720 = AaGeometry.MODES[1]
    private val m1080 = AaGeometry.MODES[2]

    @Test
    fun modesAndTheirDpi() {
        assertEquals(listOf("800x480", "1280x720", "1920x1080"), AaGeometry.MODES.map { it.toString() })
        assertEquals(140, m800.dpi)
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
    fun sixteenNineKeepsItsShape() {
        // 720 wide, 405 high: the scaler centres it, black above and below
        assertEquals(ScreenCrop.Geometry(1280, 720, 0, 0, 720, 405, 1, 1280, 720),
            AaGeometry.geometry(m720, 720, 432))
        assertEquals(ScreenCrop.Geometry(1920, 1080, 0, 0, 720, 405, 1, 1920, 1080),
            AaGeometry.geometry(m1080, 720, 432))
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
