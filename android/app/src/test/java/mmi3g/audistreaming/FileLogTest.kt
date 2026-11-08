package mmi3g.audistreaming

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

class FileLogTest {
    @get:Rule val tmp = TemporaryFolder()

    private fun old(f: File) = File(f.path + ".1")

    @Test
    fun appendsLinesAcrossInstances() {
        val f = File(tmp.root, "a.log")
        FileLog(f, 1000).line("one")
        FileLog(f, 1000).line("two")      // a new process carries on the same file
        val lines = f.readLines()
        assertEquals(2, lines.size)
        assertTrue(lines[0].endsWith(" one"))
        assertTrue(lines[1].endsWith(" two"))
    }

    @Test
    fun stampsLinesLikeLogcat() {
        val f = File(tmp.root, "a.log")
        FileLog(f, 1000).line("x")
        assertTrue(f.readText(), f.readText().matches(Regex("""\d\d-\d\d \d\d:\d\d:\d\d\.\d\d\d x\n""")))
    }

    @Test
    fun rotatesWhenFullKeepingOnePreviousFile() {
        val f = File(tmp.root, "a.log")
        val log = FileLog(f, 100)
        repeat(5) { log.line("first-" + "x".repeat(40)) }
        assertTrue(old(f).exists())
        assertTrue(old(f).readText().contains("first-"))
        assertTrue(f.length() <= 100)
        repeat(5) { log.line("second-" + "y".repeat(40)) }
        // the older generation is gone, the total stays bounded
        assertFalse(old(f).readText().contains("first-"))
        assertTrue(f.length() + old(f).length() <= 2 * 100 + 60)
    }

    @Test
    fun anUnwritableFileLosesLinesWithoutThrowing() {
        val parentIsAFile = File(tmp.newFile(), "a.log")
        FileLog(parentIsAFile, 1000).line("x")
    }
}
