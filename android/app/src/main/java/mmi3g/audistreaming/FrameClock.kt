package mmi3g.audistreaming

/**
 * The H.264 frame clock of [GlScaler]: tick k at start + k / fps, in
 * nanoseconds (System.nanoTime, the encoder's timebase). Exactly fps frames
 * a second reach the encoder, one per tick, with the tick's time as the
 * frame's: the MMI plays its stream at exactly 25 fps, and frames sent any
 * faster pile up there as lag (the encoder's own fps limit and its
 * repeat-previous-frame together made 26 fps on a Redmi Note 8 Pro).
 *
 * A tick that comes too late (the GL thread was busy) is not made up: the
 * clock goes on from the next one still ahead, so no burst follows.
 */
class FrameClock(val fps: Int, private val startNs: Long) {
    val periodNs: Long = 1_000_000_000L / fps
    private var next = 0L                 // the index of the next tick

    fun tickNs(k: Long): Long = startNs + k * periodNs

    /** Milliseconds to wait for the next tick (0: due now). */
    fun delayMs(nowNs: Long): Long {
        val d = tickNs(next) - nowNs
        return if (d <= 0) 0 else (d + 999_999) / 1_000_000
    }

    /** The due tick's time, or -1 if none is due yet; [EARLY_NS] early
     * counts as due (a Handler wakes on whole milliseconds). Ticks
     * missed are skipped. */
    fun take(nowNs: Long): Long {
        val t = tickNs(next)
        if (nowNs < t - EARLY_NS) return -1
        val behind = (nowNs - startNs) / periodNs   // the last tick at or before now
        next = maxOf(next + 1, behind + 1)
        return t
    }

    companion object {
        const val EARLY_NS = 2_000_000L
    }
}
