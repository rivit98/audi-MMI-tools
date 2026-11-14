package mmi3g.audistreaming

import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * Where Android Auto's picture is (pure, unit tested). The head unit asks
 * for one of Android Auto's video sizes ([MODES]) without margins: Android
 * Auto ignores a width margin and draws its UI over the whole width.
 * [GlScaler] scales the frame into the MMI's 720x432 H.264 picture:
 *
 * - 800x480 (5:3): whole, 0.9 both ways; Android Auto's bar is at the
 *   bottom and stays.
 * - 16:9 sizes: Android Auto draws its bar as a rail on the left, [RAIL_DP]
 *   wide (measured: 59 of 720 columns at 1280x720 / 210 dpi and at
 *   1920x1080 / 315 dpi). The rail is left out and the rest (1175x720 at
 *   1280x720) fills the picture, about 2% wider than its shape: the UI is
 *   the same size on the MMI as at 800x480. The phone's preview shows the
 *   whole frame, rail included ([GlScaler.previewWhole]).
 *
 * The video the MMI's DM642 can't show never reaches it
 * (docs/android-auto.md).
 */
object AaGeometry {
    /** A video size Android Auto offers, and the dpi asked for with it: 140
     * at 800x480, more for bigger sizes in proportion to the height, so the
     * UI's layout stays about the same, only drawn with more pixels. */
    data class Mode(val w: Int, val h: Int) {
        val dpi: Int get() = BASE_DPI * h / MODES[0].h
        /** Wider than 5:3: Android Auto draws a rail on the left. */
        val hasRail: Boolean get() = w * 3 > h * 5
        /** The rail's width in video pixels (0: none). */
        val rail: Int get() = if (hasRail) (RAIL_DP * dpi + 80) / 160 else 0
        override fun toString() = "${w}x$h"
    }

    val MODES = listOf(Mode(800, 480), Mode(1280, 720), Mode(1920, 1080))
    private const val BASE_DPI = 140
    const val RAIL_DP = 80

    /** The decoded frame without the rail, into the picW x picH picture:
     * filling it if the shape is within [MAX_STRETCH], else fitted with its
     * shape kept (the scaler centres it, black around it). */
    fun geometry(mode: Mode, picW: Int, picH: Int): ScreenCrop.Geometry {
        val keptW = mode.w - mode.rail
        val stretch = keptW.toDouble() * picH / (mode.h.toDouble() * picW)
        if (abs(stretch - 1) <= MAX_STRETCH) {
            return ScreenCrop.Geometry(mode.w, mode.h, mode.rail, 0, picW, picH, 1, keptW, mode.h)
        }
        val s = minOf(picW.toDouble() / keptW, picH.toDouble() / mode.h)
        val w = (keptW * s).roundToInt().coerceIn(2, picW)
        val h = (mode.h * s).roundToInt().coerceIn(2, picH)
        return ScreenCrop.Geometry(mode.w, mode.h, mode.rail, 0, w, h, 1, keptW, mode.h)
    }

    /** How far from its own shape the kept part may be drawn to fill the
     * picture (1175x720 into 720x432 is 2.1%). */
    const val MAX_STRETCH = 0.03

    /** A touch at (x, y) on a viewW x viewH preview showing the whole video
     * (stretched) -> video pixels for Android Auto, clamped to the video. */
    fun touch(x: Float, y: Float, viewW: Int, viewH: Int, mode: Mode): Pair<Int, Int> {
        if (viewW <= 0 || viewH <= 0) return 0 to 0
        val vx = (x * mode.w / viewW).toInt().coerceIn(0, mode.w - 1)
        val vy = (y * mode.h / viewH).toInt().coerceIn(0, mode.h - 1)
        return vx to vy
    }
}
