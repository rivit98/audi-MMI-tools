package mmi3g.audistreaming

import kotlin.math.roundToInt

/**
 * Where Android Auto's picture is (pure, unit tested). The head unit asks
 * for one of Android Auto's video sizes ([MODES]) without margins: Android
 * Auto ignores a width margin and draws its UI over the whole width, so a
 * 1:1 copy of the left 720 columns cut the UI's right edge off. [GlScaler]
 * scales the whole frame bilinear into the MMI's 720x432 H.264 picture
 * instead, keeping its shape: 800x480 (5:3) fills it at 0.9, a 16:9 size
 * gets 720x405 with black above and below. The video the MMI's DM642
 * can't show never reaches it (docs/android-auto.md).
 */
object AaGeometry {
    /** A video size Android Auto offers, and the dpi asked for with it: 140
     * at 800x480, more for bigger sizes in proportion to the height, so the
     * UI's layout stays about the same, only drawn with more pixels. */
    data class Mode(val w: Int, val h: Int) {
        val dpi: Int get() = BASE_DPI * h / MODES[0].h
        override fun toString() = "${w}x$h"
    }

    val MODES = listOf(Mode(800, 480), Mode(1280, 720), Mode(1920, 1080))
    private const val BASE_DPI = 140

    /** The whole decoded frame, scaled into a picW x picH picture with its
     * shape kept (the scaler centres the fitted picture, black around it). */
    fun geometry(mode: Mode, picW: Int, picH: Int): ScreenCrop.Geometry {
        val s = minOf(picW.toDouble() / mode.w, picH.toDouble() / mode.h)
        val w = (mode.w * s).roundToInt().coerceIn(2, picW)
        val h = (mode.h * s).roundToInt().coerceIn(2, picH)
        return ScreenCrop.Geometry(mode.w, mode.h, 0, 0, w, h, 1, mode.w, mode.h)
    }

    /** A touch at (x, y) on a viewW x viewH preview showing the whole video
     * (stretched) -> video pixels for Android Auto, clamped to the video. */
    fun touch(x: Float, y: Float, viewW: Int, viewH: Int, mode: Mode): Pair<Int, Int> {
        if (viewW <= 0 || viewH <= 0) return 0 to 0
        val vx = (x * mode.w / viewW).toInt().coerceIn(0, mode.w - 1)
        val vy = (y * mode.h / viewH).toInt().coerceIn(0, mode.h - 1)
        return vx to vy
    }
}
