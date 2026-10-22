package mmi3g.usbtx

/**
 * Portrait vs landscape: the phone's screen keeps its own shape. A screen
 * at least as wide as it is tall goes into the MMI's landscape box (under
 * its status bar, full width), a taller one into the portrait box (left
 * half of the MMI, full height, text panel beside it). Never stretched or
 * rotated; scaled to fit, even sizes.
 */
object VideoFit {
    data class Size(val w: Int, val h: Int)

    fun portrait(screenW: Int, screenH: Int): Boolean = screenW < screenH

    fun fit(screenW: Int, screenH: Int, caps: Proto.VideoCaps): Size {
        val (bw, bh) = if (portrait(screenW, screenH)) caps.portW to caps.portH else caps.landW to caps.landH
        val scale = minOf(bw.toDouble() / screenW, bh.toDouble() / screenH)
        // + a hair: 1920 * (800 / 1920.0) must not come out as 799
        val w = ((screenW * scale + 1e-6).toInt() and 1.inv()).coerceIn(2, bw and 1.inv())
        val h = ((screenH * scale + 1e-6).toInt() and 1.inv()).coerceIn(2, bh and 1.inv())
        return Size(w, h)
    }
}
