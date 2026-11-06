package mmi3g.audistreaming

/**
 * The H.264 bits the phone needs for the MMI's hardware stream (pure, unit
 * tested): MediaCodec hands out Annex-B (start codes); the MMI wants SPS and
 * PPS without them in H264_CONFIG, and pads every frame to the config's
 * frameBytes after turning start codes into 4-byte lengths
 * (src/common/csmp4.cpp make_sample), so that is what a frame costs there.
 */
object H264 {
    const val NAL_SPS = 7
    const val NAL_PPS = 8
    const val NAL_IDR = 5
    const val NAL_AUD = 9

    /** NAL units of an Annex-B buffer as (offset, length), start codes and
     * trailing zero bytes left out. */
    fun nalUnits(b: ByteArray, len: Int = b.size): List<Pair<Int, Int>> {
        val starts = ArrayList<Int>()
        var i = 0
        while (i + 2 < len) {
            if (b[i].toInt() == 0 && b[i + 1].toInt() == 0 && b[i + 2].toInt() == 1) {
                starts += i + 3
                i += 3
            } else {
                i++
            }
        }
        val out = ArrayList<Pair<Int, Int>>(starts.size)
        for ((k, s) in starts.withIndex()) {
            var e = if (k + 1 < starts.size) starts[k + 1] - 3 else len
            while (e > s && b[e - 1].toInt() == 0) e--
            if (e > s) out += s to (e - s)
        }
        return out
    }

    fun type(b: ByteArray, nal: Pair<Int, Int>): Int = b[nal.first].toInt() and 0x1f

    /** SPS and PPS from MediaCodec's codec-config buffer (csd), or null. */
    fun spsPps(csd: ByteArray, len: Int = csd.size): Pair<ByteArray, ByteArray>? {
        var sps: ByteArray? = null
        var pps: ByteArray? = null
        for (n in nalUnits(csd, len)) {
            when (type(csd, n)) {
                NAL_SPS -> if (sps == null) sps = csd.copyOfRange(n.first, n.first + n.second)
                NAL_PPS -> if (pps == null) pps = csd.copyOfRange(n.first, n.first + n.second)
            }
        }
        return if (sps != null && pps != null) sps to pps else null
    }

    /** What the frame takes in the MMI's stream before padding: 4 + length
     * per NAL unit, AUDs dropped. */
    fun sampleBytes(b: ByteArray, len: Int = b.size): Int =
        nalUnits(b, len).filter { type(b, it) != NAL_AUD }.sumOf { 4 + it.second }

    fun isIdr(b: ByteArray, len: Int = b.size): Boolean =
        nalUnits(b, len).any { type(b, it) == NAL_IDR }

    /** frameBytes after a frame of need bytes did not fit: half as much
     * again as it needed, at least twice the old, in 16 KB steps, at most
     * max; the old value if it cannot grow. */
    fun grow(current: Int, need: Int, max: Int): Int {
        val want = maxOf(need + need / 2, current * 2)
        val stepped = (want + 16383) / 16384 * 16384
        return minOf(stepped, max).coerceAtLeast(current)
    }

    /** The H.264 picture for the MMI: as wide as it allows (16-aligned),
     * the MMI screen's 5:3 shape (800x480), 16-aligned height. 720x432
     * with the MMI's 720x576 limit. */
    fun pictureSize(maxW: Int, maxH: Int): Pair<Int, Int> {
        var w = maxW / 16 * 16
        var h = (w * 3 / 5) / 16 * 16
        if (h > maxH) {
            h = maxH / 16 * 16
            w = (h * 5 / 3) / 16 * 16
        }
        return w to h
    }
}
