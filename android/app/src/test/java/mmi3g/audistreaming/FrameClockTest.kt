package mmi3g.audistreaming

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class FrameClockTest {
    private val ms = 1_000_000L

    @Test fun ticksEvery40msAt25fps() {
        val c = FrameClock(25, 1000 * ms)
        assertEquals(40 * ms, c.periodNs)
        assertEquals(1000 * ms, c.take(1000 * ms))
        assertEquals(-1, c.take(1010 * ms))                 // not yet
        assertEquals(30, c.delayMs(1010 * ms))
        assertEquals(1040 * ms, c.take(1039 * ms))          // 1 ms early counts
        assertEquals(1080 * ms, c.take(1080 * ms))
    }

    @Test fun exactlyFpsAnHourLong() {
        val c = FrameClock(25, 0)
        var now = 0L
        var frames = 0
        var last = -1L
        // a Handler that wakes a little late, sometimes much later
        var i = 0
        while (now < 3600_000 * ms) {
            val t = c.take(now)
            if (t >= 0) {
                assertTrue(t > last)
                last = t
                frames++
            }
            now += c.delayMs(now) * ms + (if (i++ % 97 == 0) 3 * ms else 300_000L)
        }
        assertTrue(frames <= 25 * 3600 + 1)
        assertTrue(frames >= 25 * 3600 * 99 / 100)
    }

    @Test fun lateTicksAreSkippedNotBurst() {
        val c = FrameClock(25, 0)
        assertEquals(0, c.take(0))
        // the GL thread was busy for 200 ms: one frame, then the next tick ahead
        assertEquals(40 * ms, c.take(200 * ms))
        assertEquals(-1, c.take(201 * ms))
        assertEquals(240 * ms, c.take(240 * ms))
    }

    @Test fun otherRates() {
        val c = FrameClock(15, 0)
        assertEquals(66_666_666L, c.periodNs)
        assertEquals(0, c.take(0))
        assertEquals(67, c.delayMs(0))
        assertEquals(66_666_666L, c.take(67 * ms))
    }
}
