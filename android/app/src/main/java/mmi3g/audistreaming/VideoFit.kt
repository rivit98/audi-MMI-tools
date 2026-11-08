package mmi3g.audistreaming

/**
 * Portrait vs landscape: the phone's screen keeps its own shape. A screen
 * at least as wide as it is tall goes into the MMI's landscape box (the
 * whole 800x480 screen), a taller one into the portrait box (left
 * half of the MMI, full height, text panel beside it). Never stretched or
 * rotated; scaled to fit, even sizes.
 */
object VideoFit {
    data class Size(val w: Int, val h: Int)

    fun portrait(screenW: Int, screenH: Int): Boolean = screenW < screenH

    fun fit(screenW: Int, screenH: Int, caps: Proto.VideoCaps): Size {
        val (bw, bh) = if (portrait(screenW, screenH)) caps.portW to caps.portH else caps.landW to caps.landH
        return fit(screenW, screenH, bw, bh)
    }

    /** The screen scaled to fit a bw x bh box, even sizes. */
    fun fit(screenW: Int, screenH: Int, bw: Int, bh: Int): Size {
        val scale = minOf(bw.toDouble() / screenW, bh.toDouble() / screenH)
        // + a hair: 1920 * (800 / 1920.0) must not come out as 799
        val w = ((screenW * scale + 1e-6).toInt() and 1.inv()).coerceIn(2, bw and 1.inv())
        val h = ((screenH * scale + 1e-6).toInt() and 1.inv()).coerceIn(2, bh and 1.inv())
        return Size(w, h)
    }

    /** How many times the picture's size the capture runs at: the largest
     * k <= MAX_FACTOR with k * picture still within the screen (1 if even
     * that is not). Android scales the screen to the capture (bilinear,
     * then under 2:1), Downscale.box does the k:1 rest. A 2340x1080 phone:
     * landscape 800x368 x2, portrait 220x480 x4. */
    fun factor(screenW: Int, screenH: Int, pic: Size): Int =
        (MAX_FACTOR downTo 2).firstOrNull { pic.w * it <= screenW && pic.h * it <= screenH } ?: 1

    const val MAX_FACTOR = 4
}
