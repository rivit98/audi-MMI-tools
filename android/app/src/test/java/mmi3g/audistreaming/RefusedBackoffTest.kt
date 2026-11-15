package mmi3g.audistreaming

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RefusedBackoffTest {
    @Test
    fun waitsThenDoublesUpToTheMax() {
        val b = RefusedBackoff(5_000, 60_000, 300_000)
        assertEquals(5_000, b.refused(1_000))
        assertFalse(b.due(5_999))
        assertTrue(b.due(6_000))
        assertEquals(10_000, b.refused(7_000))
        assertEquals(20_000, b.refused(20_000))
        assertEquals(40_000, b.refused(50_000))
        assertEquals(60_000, b.refused(100_000))
        assertEquals(60_000, b.refused(170_000))
        assertFalse(b.due(229_999))
        assertTrue(b.due(230_000))
    }

    @Test
    fun aLongRunInBetweenStartsAgain() {
        val b = RefusedBackoff(5_000, 60_000, 300_000)
        b.refused(0)
        b.refused(10_000)
        assertEquals(10_000, b.delayMs)
        assertEquals(5_000, b.refused(10_000 + 300_000))
    }

    @Test
    fun aNewConnectionStartsAgain() {
        val b = RefusedBackoff(5_000, 60_000, 300_000)
        b.refused(0)
        b.refused(6_000)
        b.reset()
        assertTrue(b.due(0))
        assertEquals(5_000, b.refused(7_000))
    }
}
