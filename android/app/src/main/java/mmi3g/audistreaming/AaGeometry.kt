package mmi3g.audistreaming

/**
 * Where Android Auto's picture is (pure, unit tested). The head unit asks
 * for 800x480, Android Auto's smallest video, without margins: Android
 * Auto ignores a width margin and draws its UI over all 800 columns, so a
 * 1:1 copy of the left 720 cut the UI's right edge off. [GlScaler] scales
 * the whole frame bilinear into the MMI's 720x432 H.264 picture instead:
 * 0.9 both ways, 5:3 kept, and the 800-wide picture the MMI's DM642 can't
 * show never reaches it (docs/android-auto.md).
 */
object AaGeometry {
    const val VIDEO_W = 800
    const val VIDEO_H = 480

    /** The whole decoded frame, scaled into the picW x picH picture. */
    fun geometry(picW: Int, picH: Int): ScreenCrop.Geometry =
        ScreenCrop.Geometry(VIDEO_W, VIDEO_H, 0, 0, picW, picH, 1, VIDEO_W, VIDEO_H)

    /** A touch at (x, y) on a viewW x viewH preview showing the whole video
     * (stretched) -> video pixels for Android Auto, clamped to the video. */
    fun touch(x: Float, y: Float, viewW: Int, viewH: Int): Pair<Int, Int> {
        if (viewW <= 0 || viewH <= 0) return 0 to 0
        val vx = (x * VIDEO_W / viewW).toInt().coerceIn(0, VIDEO_W - 1)
        val vy = (y * VIDEO_H / viewH).toInt().coerceIn(0, VIDEO_H - 1)
        return vx to vy
    }
}
