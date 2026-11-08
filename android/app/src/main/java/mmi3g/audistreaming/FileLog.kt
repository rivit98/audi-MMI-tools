package mmi3g.audistreaming

import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * A text log that survives the process: each line is appended and on disk
 * at once, stamped like logcat so the two can be read side by side. A file
 * that would pass maxBytes becomes `<file>.1` (replacing the previous one),
 * so the two together stay about 2 x maxBytes. A failed write loses the line.
 */
class FileLog(private val file: File, private val maxBytes: Long) {
    private val stamp = SimpleDateFormat("MM-dd HH:mm:ss.SSS", Locale.US)

    @Synchronized
    fun line(text: String) {
        val bytes = "${stamp.format(Date())} $text\n".toByteArray()
        try {
            if (file.length() > 0 && file.length() + bytes.size > maxBytes) {
                val old = File(file.path + ".1")
                old.delete()
                file.renameTo(old)
            }
            FileOutputStream(file, true).use { it.write(bytes) }
        } catch (_: IOException) {
        }
    }
}
