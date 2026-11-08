package mmi3g.audistreaming

import kotlin.math.max
import kotlin.math.roundToInt

/**
 * Leaving the phone's navigation bar out of the picture (pure, unit tested).
 *
 * A projection always composes the whole screen, and letterboxes it into a
 * virtual display of another shape. So the display runs at the screen's own
 * shape, scaled so that the part kept is exactly k times the picture, and the
 * pictures are made from that part only: the kept part's edges fall on capture
 * pixels, and each picture pixel is still a whole k x k block.
 */
object ScreenCrop {
    /** Screen pixels to leave out at each edge. */
    data class Insets(val left: Int = 0, val top: Int = 0, val right: Int = 0, val bottom: Int = 0) {
        fun contentW(screenW: Int) = screenW - left - right
        fun contentH(screenH: Int) = screenH - top - bottom
    }

    val NONE = Insets()

    /** The virtual display is capW x capH; the kept part starts at
     * (cropX, cropY) and is picW * k x picH * k of it. */
    data class Geometry(
        val capW: Int,
        val capH: Int,
        val cropX: Int,
        val cropY: Int,
        val picW: Int,
        val picH: Int,
        val k: Int,
    )

    /** [insets], or none if they would take too much of the screen: that
     * can't be a navigation bar. */
    fun sane(insets: Insets, screenW: Int, screenH: Int): Insets {
        val edges = listOf(insets.left, insets.top, insets.right, insets.bottom)
        if (edges.any { it < 0 }) return NONE
        if ((insets.left + insets.right) * MAX_SHARE > screenW) return NONE
        if ((insets.top + insets.bottom) * MAX_SHARE > screenH) return NONE
        return insets
    }

    /** A pic not of the kept part's exact shape (it has even sizes) loses
     * the difference, centred. */
    fun geometry(screenW: Int, screenH: Int, insets: Insets, picW: Int, picH: Int, k: Int): Geometry {
        val keptW = picW * k
        val keptH = picH * k
        val contentW = insets.contentW(screenW).toDouble()
        val contentH = insets.contentH(screenH).toDouble()
        val scale = max(keptW / contentW, keptH / contentH)
        val capW = max((screenW * scale).roundToInt(), keptW)
        val capH = max((screenH * scale).roundToInt(), keptH)
        val cropX = (insets.left * scale + (contentW * scale - keptW) / 2).roundToInt().coerceIn(0, capW - keptW)
        val cropY = (insets.top * scale + (contentH * scale - keptH) / 2).roundToInt().coerceIn(0, capH - keptH)
        return Geometry(capW, capH, cropX, cropY, picW, picH, k)
    }

    private const val MAX_SHARE = 5
}
