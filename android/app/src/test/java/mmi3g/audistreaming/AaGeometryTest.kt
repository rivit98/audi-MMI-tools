package mmi3g.audistreaming

import org.junit.Assert.assertEquals
import org.junit.Test

class AaGeometryTest {
    @Test
    fun marginsLeaveTheMmiPicture() {
        assertEquals(80 to 48, AaGeometry.margins(720, 432))
        assertEquals(0 to 0, AaGeometry.margins(900, 600))       // never negative
    }

    @Test
    fun geometryIsTheTopLeftOneToOne() {
        val g = AaGeometry.geometry(720, 432)
        assertEquals(ScreenCrop.Geometry(800, 480, 0, 0, 720, 432, 1), g)
        // the kept part: u 0..0.9, v (from the bottom) 0.1..1
        val q = GlBox.quadTex(g)
        assertEquals(0f, q[0], 1e-6f)
        assertEquals(0.9f, q[2], 1e-6f)
        assertEquals(0.1f, q[1], 1e-6f)
        assertEquals(1f, q[5], 1e-6f)
    }

    @Test
    fun touchScalesAndClamps() {
        // a 1440x864 preview of the 720x432 UI: halves
        assertEquals(360 to 216, AaGeometry.touch(720f, 432f, 1440, 864, 720, 432))
        assertEquals(0 to 0, AaGeometry.touch(-5f, -5f, 1440, 864, 720, 432))
        assertEquals(719 to 431, AaGeometry.touch(1440f, 864f, 1440, 864, 720, 432))
        assertEquals(0 to 0, AaGeometry.touch(10f, 10f, 0, 0, 720, 432))
    }
}
