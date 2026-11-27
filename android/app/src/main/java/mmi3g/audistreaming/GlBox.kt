package mmi3g.audistreaming

/**
 * The pure half of [GlScaler]'s drawing: a k x k box filter on the GPU (k = 1
 * is a 1:1 copy), or a bilinear scale of a kept part of another size
 * ([FrameGeometry.scaled]).
 */
object GlBox {
    /** Even k: bilinear sampling, one tap at the corner shared by each 2x2
     * group (= their average). Odd k: nearest, one tap per pixel. */
    fun linear(k: Int): Boolean = k % 2 == 0

    /** [linear] for g; a [FrameGeometry.scaled] kept part is always
     * bilinear (nearest would drop whole columns). */
    fun linear(g: FrameGeometry): Boolean = g.scaled || linear(g.k)

    /** Where to sample, in input pixels from the output pixel's centre
     * (the centre of its k x k block): (k/2)^2 taps for even k, k^2 odd. */
    fun taps(k: Int): List<Pair<Double, Double>> {
        require(k in 1..MAX_FACTOR) { "factor $k" }
        val c = (k - 1) / 2.0
        val pos = if (linear(k)) (0 until k step 2).map { it + 0.5 - c } else (0 until k).map { it - c }
        return pos.flatMap { y -> pos.map { x -> x to y } }
    }

    const val VERTEX_SHADER = """
attribute vec4 aPos;
attribute vec4 aTex;
uniform mat4 uTexMatrix;
varying highp vec2 vTex;
void main() {
    gl_Position = aPos;
    vTex = (uTexMatrix * aTex).xy;
}
"""

    /** The fragment shader for k: the taps unrolled. uStepX / uStepY are
     * one input pixel across / down in texture coordinates (through the
     * SurfaceTexture's transform). highp: mediump's 10-bit mantissa can't
     * tell 2880 pixels apart. */
    fun fragmentShader(k: Int): String = fragmentShader(taps(k))

    /** The shader for g: k:1 box taps, or for a [FrameGeometry.scaled]
     * one [scaledTaps]. */
    fun fragmentShader(g: FrameGeometry): String =
        fragmentShader(if (g.scaled) scaledTaps(g) else taps(g.k))

    /** A scaled picture (any factor): n x n bilinear taps spread evenly over
     * the input pixels one picture pixel covers, n = that footprint
     * rounded (at least 1), so a shrink by more than ~1.5 averages what
     * a single tap would skip (thin text). In input pixels from the
     * output pixel's centre, as [taps]. */
    fun scaledTaps(g: FrameGeometry): List<Pair<Double, Double>> {
        val fx = g.keptW.toDouble() / g.picW
        val fy = g.keptH.toDouble() / g.picH
        val n = maxOf(1, Math.round(maxOf(fx, fy)).toInt()).coerceAtMost(MAX_FACTOR)
        fun spread(f: Double) = (0 until n).map { (it + 0.5) / n * f - f / 2 }
        val xs = spread(fx)
        return spread(fy).flatMap { y -> xs.map { x -> x to y } }
    }

    private fun fragmentShader(t: List<Pair<Double, Double>>): String {
        val sb = StringBuilder()
        sb.append("#extension GL_OES_EGL_image_external : require\n")
        sb.append("precision highp float;\n")
        sb.append("varying highp vec2 vTex;\n")
        sb.append("uniform samplerExternalOES uTex;\n")
        sb.append("uniform highp vec2 uStepX;\n")
        sb.append("uniform highp vec2 uStepY;\n")
        sb.append("void main() {\n")
        sb.append("    vec3 s = vec3(0.0);\n")
        for ((x, y) in t) {
            sb.append("    s += texture2D(uTex, vTex + uStepX * (${glFloat(x)}) + uStepY * (${glFloat(y)})).rgb;\n")
        }
        sb.append("    gl_FragColor = vec4(s * ${glFloat(1.0 / t.size)}, 1.0);\n")
        sb.append("}\n")
        return sb.toString()
    }

    /** One input pixel across and down, in texture coordinates: the
     * linear part of the SurfaceTexture's column-major transform applied
     * to (1/srcW, 0) and (0, 1/srcH). Returns (xx, xy, yx, yy). */
    fun steps(m: FloatArray, srcW: Int, srcH: Int): FloatArray {
        val dx = 1f / srcW
        val dy = 1f / srcH
        return floatArrayOf(m[0] * dx, m[1] * dx, m[4] * dy, m[5] * dy)
    }

    /** Texture coordinates of [FrameGeometry]'s kept part for the
     * full-viewport triangle strip: bottom left, bottom right, top left, top
     * right, before the SurfaceTexture's transform (y up from the bottom). */
    fun quadTex(g: FrameGeometry): FloatArray {
        val u0 = g.cropX.toFloat() / g.capW
        val u1 = (g.cropX + g.keptW).toFloat() / g.capW
        val v0 = 1f - (g.cropY + g.keptH).toFloat() / g.capH
        val v1 = 1f - g.cropY.toFloat() / g.capH
        return floatArrayOf(u0, v0, u1, v0, u0, v1, u1, v1)
    }

    // GLSL ES 1.0 wants a decimal point in a float literal
    private fun glFloat(v: Double): String {
        val s = "%.8f".format(java.util.Locale.ROOT, v).trimEnd('0')
        return if (s.endsWith('.')) s + "0" else s
    }

    const val MAX_FACTOR = 4
}
