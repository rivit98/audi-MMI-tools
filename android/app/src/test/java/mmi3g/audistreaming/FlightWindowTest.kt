package mmi3g.audistreaming

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class FlightWindowTest {
    @Test
    fun takesUpToMaxThenAnswersMakeRoom() {
        val w = FlightWindow(3000)
        assertTrue(w.take(2, 0))
        assertTrue(w.hasRoom(2))
        assertTrue(w.take(2, 0))
        assertFalse(w.hasRoom(2))
        assertFalse(w.take(2, 0))
        w.done(1, 10)
        assertTrue(w.hasRoom(2))
    }

    @Test
    fun aBatchFreesManyAtOnce() {
        val w = FlightWindow(3000)
        repeat(12) { assertTrue(w.take(12, 0)) }
        assertFalse(w.hasRoom(12))
        w.done(5, 10)
        repeat(5) { assertTrue(w.take(12, 10)) }
        assertFalse(w.take(12, 10))
    }

    @Test
    fun givesUpAfterTimeoutWithoutAnswers() {
        val w = FlightWindow(3000)
        w.take(2, 1000)
        w.take(2, 1500)
        assertEquals(0, w.expire(3999))
        assertEquals(2, w.expire(4000))
        assertEquals(0, w.expire(9000))
        assertTrue(w.hasRoom(1))
    }

    @Test
    fun answersPutTheTimeoutOff() {
        val w = FlightWindow(3000)
        repeat(3) { w.take(12, 0) }
        w.done(1, 2500)
        assertEquals(0, w.expire(5499))
        assertEquals(2, w.expire(5500))
    }

    @Test
    fun anIdleWindowDoesNotExpireTheNextPicture() {
        val w = FlightWindow(3000)
        w.take(2, 0)
        w.done(1, 10)
        assertTrue(w.take(2, 60000))
        assertEquals(0, w.expire(60001))
    }

    @Test
    fun lateAnswersStopAtZero() {
        val w = FlightWindow(3000)
        w.take(2, 0)
        w.clear()
        w.done(5, 10)
        assertTrue(w.take(1, 20))
        assertFalse(w.take(1, 20))
    }
}
