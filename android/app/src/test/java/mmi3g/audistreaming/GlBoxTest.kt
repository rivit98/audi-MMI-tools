package mmi3g.audistreaming

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GlBoxTest {
    @Test
    fun factorForTheRedmiNote8Pro() {
        // landscape 2340x1080 -> 720x432 holds 720x332: x3 = 2160x996 fits, x4 = 2880 doesn't
        assertEquals(3, GlBox.factor(2340, 1080, 720, 432))
        // portrait 1080x2340 -> 199x432: x4 = 797x1728 fits
        assertEquals(4, GlBox.factor(1080, 2340, 720, 432))
    }

    @Test
    fun factorOneWhenThePictureIsAsBigAsTheScreen() {
        assertEquals(1, GlBox.factor(720, 432, 720, 432))
        assertEquals(1, GlBox.factor(1000, 600, 720, 432))
        assertEquals(2, GlBox.factor(1440, 864, 720, 432))
        assertEquals(1, GlBox.factor(0, 0, 720, 432))
    }

    /** Every capture pixel of the k x k block counts once: with bilinear
     * taps (even k) at a corner, each tap covers the 2x2 pixels around it. */
    @Test
    fun tapsCoverTheBlockExactlyOnce() {
        for (k in 1..GlBox.MAX_FACTOR) {
            val c = (k - 1) / 2.0
            val hits = Array(k) { IntArray(k) }
            for ((x, y) in GlBox.taps(k)) {
                val cover = if (GlBox.linear(k)) listOf(-0.5, 0.5) else listOf(0.0)
                for (dy in cover) for (dx in cover) {
                    val px = x + dx + c
                    val py = y + dy + c
                    // a pixel centre exactly
                    assertEquals(Math.rint(px), px, 1e-9)
                    assertEquals(Math.rint(py), py, 1e-9)
                    hits[py.toInt()][px.toInt()]++
                }
            }
            for (row in hits) for (n in row) assertEquals("k=$k", 1, n)
        }
    }

    @Test
    fun tapCounts() {
        assertEquals(listOf(0.0 to 0.0), GlBox.taps(1))
        assertEquals(listOf(0.0 to 0.0), GlBox.taps(2))   // one bilinear tap = the 2x2 average
        assertEquals(9, GlBox.taps(3).size)
        assertEquals(4, GlBox.taps(4).size)
        assertTrue(GlBox.linear(2) && GlBox.linear(4))
        assertFalse(GlBox.linear(1) || GlBox.linear(3))
    }

    @Test
    fun shaderHasOneLookupPerTapAndGlslFloats() {
        val fs = GlBox.fragmentShader(3)
        assertEquals(9, Regex("texture2D").findAll(fs).count())
        assertTrue(fs.contains("(-1.0)") && fs.contains("(0.0)") && fs.contains("(1.0)"))
        assertTrue(fs.contains("s * 0.11111111"))
        assertTrue(fs.startsWith("#extension GL_OES_EGL_image_external : require"))
        val fs4 = GlBox.fragmentShader(4)
        assertEquals(4, Regex("texture2D").findAll(fs4).count())
        assertTrue(fs4.contains("s * 0.25,"))
        // no bare integers or exponents GLSL ES 1.0 would reject
        assertFalse(Regex("\\* \\(-?\\d+\\)").containsMatchIn(fs + fs4))
        assertFalse(fs.contains("E-"))
    }

    @Test
    fun stepsFollowTheTransform() {
        // SurfaceTexture's usual matrix for a virtual display: y flipped
        val m = floatArrayOf(1f, 0f, 0f, 0f, 0f, -1f, 0f, 0f, 0f, 0f, 1f, 0f, 0f, 1f, 0f, 1f)
        val s = GlBox.steps(m, 2160, 1296)
        assertEquals(1f / 2160, s[0], 1e-9f)
        assertEquals(0f, s[1], 0f)
        assertEquals(0f, s[2], 0f)
        assertEquals(-1f / 1296, s[3], 1e-9f)
    }
}
