package mmi3g.audistreaming

/**
 * Where Android Auto's picture is (pure, unit tested). The head unit asks
 * for 800x480, Android Auto's smallest video, with margins: the phone
 * draws its UI in the top-left (800 - margin) x (480 - margin) of each
 * frame and leaves the rest black (open-headunit measured the anchoring on
 * hardware: top-left, not centred). With margins 80 x 48 the UI is exactly
 * the MMI's 720x432 H.264 picture, so [GlScaler] copies it 1:1 (k = 1, no
 * scaling, sharp text) and the 800-wide picture the MMI's DM642 can't show
 * never reaches it (docs/android-auto.md).
 */
object AaGeometry {
    const val VIDEO_W = 800
    const val VIDEO_H = 480

    /** The margins for a picW x picH picture (at most the video). */
    fun margins(picW: Int, picH: Int): Pair<Int, Int> =
        (VIDEO_W - picW.coerceIn(2, VIDEO_W)) to (VIDEO_H - picH.coerceIn(2, VIDEO_H))

    /** The UI's part of each decoded frame, for [GlScaler]. */
    fun geometry(picW: Int, picH: Int): ScreenCrop.Geometry =
        ScreenCrop.Geometry(VIDEO_W, VIDEO_H, 0, 0, picW.coerceIn(2, VIDEO_W), picH.coerceIn(2, VIDEO_H), 1)

    /** A touch at (x, y) on a viewW x viewH preview showing the UI (picW x
     * picH, stretched) -> video pixels for Android Auto, clamped to the UI. */
    fun touch(x: Float, y: Float, viewW: Int, viewH: Int, picW: Int, picH: Int): Pair<Int, Int> {
        if (viewW <= 0 || viewH <= 0) return 0 to 0
        val vx = (x * picW / viewW).toInt().coerceIn(0, picW - 1)
        val vy = (y * picH / viewH).toInt().coerceIn(0, picH - 1)
        return vx to vy
    }
}
