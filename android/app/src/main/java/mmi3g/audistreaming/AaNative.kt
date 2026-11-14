package mmi3g.audistreaming

/**
 * One Android Auto head unit session in libaa_jni.so: the car app's
 * src/android_auto session (protocol, TLS, services), through JNI
 * (app/src/main/cpp/aa_jni.cpp). Single-threaded: every call from the same
 * thread ([AaService]'s), and the [Callbacks] come back on it during the
 * calls ([rx], [tick], [start], [touch], [bye]).
 */
class AaNative(callbacks: Callbacks, audio: Boolean, audioMedia: Boolean, marginW: Int, marginH: Int,
               name: String) : AutoCloseable {
    interface Callbacks {
        /** Bytes for the phone, in order. */
        fun send(data: ByteArray)
        fun phase(phase: Int, detail: String)
        /** H.264 Annex-B: the codec config (SPS/PPS) or a frame. */
        fun video(data: ByteArray, tsUs: Long, config: Boolean)
        /** PCM on an audio channel ([CH_SYSTEM_AUDIO] ...). */
        fun audio(channel: Int, data: ByteArray)
    }

    private var handle = nativeCreate(callbacks, audio, audioMedia, marginW, marginH, 0, name)

    /** VERSION_REQUEST; false: Failed ([error]). */
    fun start(cert: String, key: String): Boolean = nativeStart(handle, cert, key)
    fun rx(data: ByteArray, n: Int) = nativeRx(handle, data, n)
    /** Pings; the ms until the next tick is due, at most [limitMs]. */
    fun tick(limitMs: Int): Int = nativeTick(handle, limitMs)
    /** [action] PointerAction ([TOUCH_DOWN] ...), coordinates in video pixels. */
    fun touch(action: Int, index: Int, xs: IntArray, ys: IntArray, ids: IntArray) =
        nativeTouch(handle, action, index, xs, ys, ids)
    fun bye() = nativeBye(handle)
    val phase: Int get() = nativePhase(handle)
    val phone: String get() = nativeText(handle, 0)
    val suite: String get() = nativeText(handle, 1)
    val error: String get() = nativeText(handle, 2)
    /** See aa_jni.cpp nativeStats. */
    fun stats(): LongArray = nativeStats(handle)

    override fun close() {
        if (handle != 0L) nativeDestroy(handle)
        handle = 0L
    }

    companion object {
        init { System.loadLibrary("aa_jni") }

        // session.h Phase
        const val PHASE_IDLE = 0
        const val PHASE_VERSION = 1
        const val PHASE_HANDSHAKE = 2
        const val PHASE_DISCOVERY = 3
        const val PHASE_RUNNING = 4
        const val PHASE_ENDED = 5
        const val PHASE_FAILED = 6
        val PHASE_NAMES = listOf("idle", "version", "TLS handshake", "service discovery", "running", "ended", "failed")

        // session.h channels
        const val CH_MEDIA_AUDIO = 4
        const val CH_GUIDANCE_AUDIO = 5
        const val CH_SYSTEM_AUDIO = 6

        // TouchEvent.proto PointerAction
        const val TOUCH_DOWN = 0
        const val TOUCH_UP = 1
        const val TOUCH_MOVED = 2
        const val TOUCH_POINTER_DOWN = 5
        const val TOUCH_POINTER_UP = 6

        /** The session's log lines since the last call. */
        fun drainLog(): Array<String> = nativeDrainLog() ?: emptyArray()

        @JvmStatic private external fun nativeCreate(cb: Callbacks, audio: Boolean, audioMedia: Boolean,
                                                     marginW: Int, marginH: Int, videoFps: Int, name: String): Long
        @JvmStatic private external fun nativeStart(h: Long, cert: String, key: String): Boolean
        @JvmStatic private external fun nativeRx(h: Long, data: ByteArray, n: Int)
        @JvmStatic private external fun nativeTick(h: Long, limitMs: Int): Int
        @JvmStatic private external fun nativeTouch(h: Long, action: Int, index: Int, xs: IntArray, ys: IntArray, ids: IntArray)
        @JvmStatic private external fun nativeBye(h: Long)
        @JvmStatic private external fun nativePhase(h: Long): Int
        @JvmStatic private external fun nativeStats(h: Long): LongArray
        @JvmStatic private external fun nativeText(h: Long, which: Int): String
        @JvmStatic private external fun nativeDestroy(h: Long)
        @JvmStatic private external fun nativeDrainLog(): Array<String>?
    }
}
