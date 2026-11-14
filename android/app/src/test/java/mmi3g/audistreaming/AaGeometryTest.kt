package mmi3g.audistreaming

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class AaGeometryTest {
    @Test
    fun geometryScalesTheWholeFrame() {
        val g = AaGeometry.geometry(720, 432)
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
    fun touchScalesToTheVideoAndClamps() {
        // a 1600x960 preview of the 800x480 video: halves
        assertEquals(400 to 240, AaGeometry.touch(800f, 480f, 1600, 960))
        // the preview at the MMI's 720x432: 0.9 -> 1/0.9
        assertEquals(400 to 240, AaGeometry.touch(360f, 216f, 720, 432))
        assertEquals(0 to 0, AaGeometry.touch(-5f, -5f, 1600, 960))
        assertEquals(799 to 479, AaGeometry.touch(1600f, 960f, 1600, 960))
        assertEquals(0 to 0, AaGeometry.touch(10f, 10f, 0, 0))
    }
}
