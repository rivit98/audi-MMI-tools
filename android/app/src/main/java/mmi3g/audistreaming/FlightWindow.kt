package mmi3g.audistreaming

/**
 * How many pictures the MMI has not answered yet. VIDEO_ACKS are counts, so a
 * count is all this needs. Every call may come from another thread.
 *
 * A picture the MMI never answers must not hold its place for ever, so when
 * pictures are in flight and nothing has come back for [timeoutMs], counting
 * starts again from zero. An answer after that, or after [clear], finds less
 * in flight than it says: the count stops at zero.
 */
class FlightWindow(private val timeoutMs: Long) {
    private var count = 0
    private var progressAt = 0L   // the last answer, or the first picture sent after none

    @Synchronized
    fun hasRoom(max: Int): Boolean = count < max

    /** One more in flight; false if [max] already are. */
    @Synchronized
    fun take(max: Int, now: Long): Boolean {
        if (count >= max) return false
        if (count == 0) progressAt = now
        count++
        return true
    }

    /** The MMI answered [n] pictures. */
    @Synchronized
    fun done(n: Int, now: Long) {
        count = maxOf(0, count - n)
        progressAt = now
    }

    /** How many were given up on, 0 if none. */
    @Synchronized
    fun expire(now: Long): Int {
        if (count == 0 || now - progressAt < timeoutMs) return 0
        return count.also { count = 0 }
    }

    @Synchronized
    fun clear() {
        count = 0
    }
}
