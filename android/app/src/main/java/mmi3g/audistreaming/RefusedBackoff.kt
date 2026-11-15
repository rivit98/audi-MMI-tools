package mmi3g.audistreaming

/**
 * When to try H.264 again after the MMI's REFUSED. The MMI restarts its own
 * stream after an io-media error and says REFUSED only when errors come too
 * often (or a config can't be used), so the phone waits before it sends its
 * config again: [firstMs], doubling with each REFUSED up to [maxMs]. A
 * REFUSED more than [resetAfterMs] after the one before (H.264 ran fine in
 * between) starts from [firstMs] again, as does [reset] (a new connection).
 * Times are elapsedRealtime ms. Confined to the owner's thread.
 */
class RefusedBackoff(
    private val firstMs: Long = 5_000,
    private val maxMs: Long = 60_000,
    private val resetAfterMs: Long = 300_000,
) {
    /** The wait before the next try. */
    var delayMs = 0L
        private set
    private var lastAt = 0L
    private var retryAt = 0L
    private var any = false

    /** A REFUSED at [now]: returns the wait before H.264 is tried again. */
    fun refused(now: Long): Long {
        delayMs = if (any && now - lastAt < resetAfterMs) (delayMs * 2).coerceAtMost(maxMs) else firstMs
        any = true
        lastAt = now
        retryAt = now + delayMs
        return delayMs
    }

    /** Time to try again. */
    fun due(now: Long): Boolean = now >= retryAt

    /** A new connection: the next REFUSED waits [firstMs]. */
    fun reset() {
        any = false
        delayMs = 0L
        retryAt = 0L
    }
}
