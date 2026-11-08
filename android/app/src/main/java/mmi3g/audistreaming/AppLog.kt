package mmi3g.audistreaming

import android.app.ActivityManager
import android.content.Context
import android.os.Build
import android.util.Log
import java.io.File

/**
 * The app's diagnostics: to logcat, whose buffers on a phone hold little, and
 * to [FileLog] in the app's external files directory, to pull after a drive:
 * adb pull /sdcard/Android/data/mmi3g.audistreaming/files/audi_streaming.log
 *
 * Starts each process with the app's version, the phone and why Android
 * ended the app's previous processes.
 */
object AppLog {
    private const val TAG = "AudiStreaming"
    private const val FILE_NAME = "audi_streaming.log"
    private const val MAX_BYTES = 1024 * 1024L
    private const val EXIT_REASONS = 3

    @Volatile private var file: FileLog? = null

    @Synchronized
    fun init(context: Context) {
        if (file != null) return
        val dir = context.getExternalFilesDir(null) ?: context.filesDir
        file = FileLog(File(dir, FILE_NAME), MAX_BYTES)
        val version = context.packageManager.getPackageInfo(context.packageName, 0).versionName
        i("--- app start: build $version, ${Build.MANUFACTURER} ${Build.MODEL}, " +
            "Android ${Build.VERSION.RELEASE} (${Build.DISPLAY})")
        if (Build.VERSION.SDK_INT >= 30) {
            val am = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            for (e in am.getHistoricalProcessExitReasons(context.packageName, 0, EXIT_REASONS)) {
                i("earlier exit at ${e.timestamp} (ms since 1970): reason ${e.reason}, " +
                    "importance ${e.importance}, ${e.description}")
            }
        }
    }

    fun i(text: String) {
        Log.i(TAG, text)
        file?.line(text)
    }

    fun w(text: String, e: Throwable? = null) {
        Log.w(TAG, text, e)
        file?.line("W $text" + if (e != null) ": $e" else "")
    }
}
