package mmi3g.audistreaming

/**
 * The phone's screen in the MMI's picture: it keeps its own shape, never
 * stretched or rotated; scaled to fit, even sizes.
 */
object VideoFit {
    data class Size(val w: Int, val h: Int)

    /** A screen taller than it is wide (a square one counts as landscape). */
    fun portrait(screenW: Int, screenH: Int): Boolean = screenW < screenH

    /** The screen scaled to fit a bw x bh box, even sizes. */
    fun fit(screenW: Int, screenH: Int, bw: Int, bh: Int): Size {
        val scale = minOf(bw.toDouble() / screenW, bh.toDouble() / screenH)
        // + a hair: 1920 * (800 / 1920.0) must not come out as 799
        val w = ((screenW * scale + 1e-6).toInt() and 1.inv()).coerceIn(2, bw and 1.inv())
        val h = ((screenH * scale + 1e-6).toInt() and 1.inv()).coerceIn(2, bh and 1.inv())
        return Size(w, h)
    }
}
