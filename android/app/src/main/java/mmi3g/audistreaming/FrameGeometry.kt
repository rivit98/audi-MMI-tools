package mmi3g.audistreaming

/**
 * Which part of Android Auto's decoded frame goes into the MMI's picture
 * ([AaGeometry] makes it, [GlScaler] draws it). The decoded frame is
 * capW x capH; the kept part starts at (cropX, cropY) and is keptW x keptH:
 * picW * k x picH * k (k:1 box filter, [GlBox]; k = 1 is a 1:1 copy), or any
 * other size, then [scaled] bilinear into the picture (800x480 -> 720x432).
 */
data class FrameGeometry(
    val capW: Int,
    val capH: Int,
    val cropX: Int,
    val cropY: Int,
    val picW: Int,
    val picH: Int,
    val k: Int,
    val keptW: Int = picW * k,
    val keptH: Int = picH * k,
) {
    val scaled: Boolean get() = keptW != picW * k || keptH != picH * k
}
