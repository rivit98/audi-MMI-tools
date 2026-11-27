package mmi3g.audistreaming

/** What the Android Auto mode ([AaService]) and the activity share: the MMI
 * link and the H.264 settings ([StreamOptions]). */
object Streaming {
    /** The open link to the MMI, set by the activity. */
    @Volatile var link: AccessoryLink? = null
    /** Frames a second at most, set by the activity (its slider). */
    @Volatile var maxFps = DEFAULT_FPS
    /** H.264 bits a second, set by the activity (its drop-down). */
    @Volatile var bitRate = DEFAULT_BIT_RATE
    /** Hold the screen on while Android Auto runs, set by the activity (its checkbox). */
    @Volatile var keepScreenOn = false

    const val DEFAULT_FPS = 25
    const val MIN_FPS = 1
    const val MAX_FPS = 25            // the MMI's stream runs at 25
    // H.264 bit rates of the drop-down (res/values/strings.xml bit_rates);
    // 2.5 Mbit/s is ~12 KB a frame at 25 fps
    // no 8 Mbit/s: its IDRs (up to ~170 KB at 720x432) outgrow the MMI's
    // 128 KB frames
    val BIT_RATES = intArrayOf(2_500_000, 4_000_000, 6_000_000)
    const val DEFAULT_BIT_RATE = 4_000_000
}
