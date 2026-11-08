package mmi3g.audistreaming

/**
 * The pure half of [GlScaler]: a k x k box filter on the GPU.
 *
 * Without it the virtual display draws straight into the encoder at
 * 720x432, and Android scales the 2340x1080 screen down 3.25:1 in one
 * bilinear step: each output pixel looks at 2x2 of the ~3x3 screen pixels
 * it covers, so thin text strokes are hit or missed (blurry H.264 text). With it the virtual display runs at k times the picture,
 * Android's step is about 1:1 (2340 -> 2160 for k = 3), and every output
 * pixel is the average of the k x k capture pixels under it - what
 * Downscale.box does for the JPEG/PNG pictures.
 */
object GlBox {
    /** The largest k <= MAX_FACTOR with k times the mirrored screen still
     * within the screen (1 if even 2 is not). The mirror letterboxes the
     * screen into the w x h picture, so that is the content k multiplies:
     * 2340x1080 into 720x432 is 720x332, k = 3; portrait 199x432, k = 4. */
    fun factor(screenW: Int, screenH: Int, w: Int, h: Int): Int {
        if (screenW <= 0 || screenH <= 0 || w <= 0 || h <= 0) return 1
        val s = minOf(w.toDouble() / screenW, h.toDouble() / screenH)
        val cw = screenW * s
        val ch = screenH * s
        return (MAX_FACTOR downTo 2).firstOrNull { cw * it <= screenW + 0.5 && ch * it <= screenH + 0.5 } ?: 1
    }

    /** Even k: bilinear sampling, one tap at the corner shared by each 2x2
     * group (= their average). Odd k: nearest, one tap per pixel. */
    fun linear(k: Int): Boolean = k % 2 == 0

    /** Where to sample, in capture pixels from the output pixel's centre
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
     * one capture pixel across / down in texture coordinates (through the
     * SurfaceTexture's transform). highp: mediump's 10-bit mantissa can't
     * tell 2880 pixels apart. */
    fun fragmentShader(k: Int): String {
        val t = taps(k)
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

    /** One capture pixel across and down, in texture coordinates: the
     * linear part of the SurfaceTexture's column-major transform applied
     * to (1/srcW, 0) and (0, 1/srcH). Returns (xx, xy, yx, yy). */
    fun steps(m: FloatArray, srcW: Int, srcH: Int): FloatArray {
        val dx = 1f / srcW
        val dy = 1f / srcH
        return floatArrayOf(m[0] * dx, m[1] * dx, m[4] * dy, m[5] * dy)
    }

    // GLSL ES 1.0 wants a decimal point in a float literal
    private fun glFloat(v: Double): String {
        val s = "%.8f".format(java.util.Locale.ROOT, v).trimEnd('0')
        return if (s.endsWith('.')) s + "0" else s
    }

    const val MAX_FACTOR = 4
}
