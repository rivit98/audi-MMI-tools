package mmi3g.audistreaming

/**
 * k:1 box downscale of a captured screen: every output pixel is the
 * average of a k x k block. The capture runs at k times the MMI's size
 * (VideoFit.factor), so Android's own bilinear scaling only has a small
 * step left (under 2:1) and this one does the rest without dropping
 * pixels - one bilinear step of 3-5:1 skipped most of each thin text
 * stroke (blurred, broken fonts on the car, 2026-10-30).
 *
 * Pixels are 4 bytes in any byte order (the ImageReader's RGBA read as
 * native ints); each byte is averaged on its own and the top byte (alpha
 * in RGBA on a little-endian phone) is set to 0xff. k <= 16, so a byte's
 * sum fits its 16-bit lane.
 */
object Downscale {
    const val MAX_FACTOR = 16

    /** src: rows of srcStride pixels, at least outH * k of them; out gets
     * outW x outH pixels, stride outW. */
    fun box(src: IntArray, srcStride: Int, k: Int, outW: Int, outH: Int, out: IntArray) {
        require(k in 1..MAX_FACTOR) { "factor $k" }
        require(outW * k <= srcStride && src.size >= (outH * k - 1) * srcStride + outW * k)
        require(out.size >= outW * outH)
        if (k == 1) {
            for (y in 0 until outH) {
                val s = y * srcStride
                val o = y * outW
                for (x in 0 until outW) out[o + x] = src[s + x] or OPAQUE
            }
            return
        }
        val n = k * k
        val half = n / 2
        for (y in 0 until outH) {
            val row0 = y * k * srcStride
            val o = y * outW
            for (x in 0 until outW) {
                var even = 0          // bytes 0 and 2, one 16-bit lane each
                var odd = 0           // bytes 1 and 3
                var s = row0 + x * k
                for (dy in 0 until k) {
                    for (dx in 0 until k) {
                        val p = src[s + dx]
                        even += p and 0x00ff00ff
                        odd += (p ushr 8) and 0x00ff00ff
                    }
                    s += srcStride
                }
                val b0 = ((even and 0xffff) + half) / n
                val b1 = ((odd and 0xffff) + half) / n
                val b2 = ((even ushr 16) + half) / n
                out[o + x] = OPAQUE or (b2 shl 16) or (b1 shl 8) or b0
            }
        }
    }

    private const val OPAQUE = 0xff shl 24
}
