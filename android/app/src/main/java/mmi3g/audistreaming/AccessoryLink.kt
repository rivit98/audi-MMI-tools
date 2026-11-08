package mmi3g.audistreaming

import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.util.Log
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * One open USB accessory (the MMI): a reader thread decoding frames and a
 * single-thread writer. Every callback is delivered on the main thread.
 *
 * Any I/O error closes the link and reports [Listener.onClosed]; the
 * activity then goes back to waiting. A link is single-use.
 *
 * Video: the MMI announces what it can show in VIDEO_CAPS ([videoCaps],
 * null for an MMI app without video). [sendPicture] keeps at most
 * [MAX_IN_FLIGHT] pictures in flight: one being decoded and drawn on the
 * MMI, the next already there waiting, so the MMI never sits idle while
 * the phone captures, compresses and sends. The MMI keeps only the newest
 * waiting picture, so nothing queues up. A picture counts as done when
 * the MMI acks it (shown, bad or dropped). Only how many are in flight is
 * kept ([FlightWindow]): acks are counts, for no id in particular.
 *
 * H.264 (format FMT_H264): the MMI counts each frame as done as soon as it
 * is in its stream and acks them in batches, so up to [H264_IN_FLIGHT] are
 * allowed; what the MMI says about the stream (H264_STATUS) lands in [h264]
 * for the capture service to pick up.
 */
class AccessoryLink(
    private val usb: UsbManager,
    private val accessory: UsbAccessory,
    private val listener: Listener,
) {
    interface Listener {
        fun onPeerHello(peer: String)
        fun onClosed(closed: AccessoryLink, reason: String)
    }

    private val main = Handler(Looper.getMainLooper())
    private val writer: ExecutorService = Executors.newSingleThreadExecutor()
    private var pfd: ParcelFileDescriptor? = null
    private var output: FileOutputStream? = null
    @Volatile private var closed = false

    @Volatile var videoCaps: Proto.VideoCaps? = null
        private set
    /** Counters for the UI; written by the reader / capture threads. */
    class VideoStats {
        @Volatile var sent = 0
        @Volatile var shown = 0
        @Volatile var bad = 0
        @Volatile var dropped = 0
        @Volatile var bytes = 0
        @Volatile var w = 0
        @Volatile var h = 0
    }
    val video = VideoStats()

    /** H264_STATUS from the MMI, consumed by the capture service. */
    class H264State {
        val sync = java.util.concurrent.atomic.AtomicBoolean(false)
        val configWanted = java.util.concurrent.atomic.AtomicBoolean(false)
        val refused = java.util.concurrent.atomic.AtomicBoolean(false)
        @Volatile var playing = false
    }
    val h264 = H264State()
    private val inFlight = FlightWindow(ACK_TIMEOUT_MS)

    val description: String get() = "${accessory.manufacturer} ${accessory.model}"

    /** Opens the accessory; false if the system refused (no permission / gone). */
    fun open(): Boolean {
        val fd = try {
            usb.openAccessory(accessory)
        } catch (e: SecurityException) {
            Log.w(TAG, "openAccessory", e)
            null
        } ?: return false
        pfd = fd
        output = FileOutputStream(fd.fileDescriptor)
        val input = FileInputStream(fd.fileDescriptor)
        Thread({ readLoop(input) }, "aoa-reader").apply { isDaemon = true }.start()
        write(Proto.encode(Proto.HELLO, HELLO.toByteArray()))
        return true
    }

    /** True if [sendPicture] would take a picture now. */
    fun readyForPicture(maxInFlight: Int = MAX_IN_FLIGHT): Boolean {
        if (closed) return false
        expire()
        return inFlight.hasRoom(maxInFlight)
    }

    private fun expire() {
        val lost = inFlight.expire(SystemClock.elapsedRealtime())
        if (lost > 0) Log.w(TAG, "$lost pictures never acked")
    }

    /** Queues a picture (format Proto.FMT_*); false if maxInFlight are
     * still in flight. */
    fun sendPicture(id: Int, w: Int, h: Int, format: Int, data: ByteArray, phoneMs: Int,
                    maxInFlight: Int = MAX_IN_FLIGHT): Boolean {
        if (closed) return false
        expire()
        if (!inFlight.take(maxInFlight, SystemClock.elapsedRealtime())) return false
        video.sent++
        video.bytes = data.size
        video.w = w
        video.h = h
        write(Proto.picture(id, w, h, format, data, phoneMs))
        return true
    }

    fun sendVideoStop() {
        inFlight.clear()
        write(Proto.encode(Proto.VIDEO_STOP, ByteArray(0)))
    }

    /** H264_CONFIG (Proto.h264Config): starts or restarts the MMI's stream. */
    fun sendH264Config(frame: ByteArray) {
        h264.playing = false
        write(frame)
    }

    /** The MMI's status bar over a landscape picture on / off. */
    fun sendDetails(show: Boolean) {
        write(Proto.videoDetails(show))
    }

    fun close(reason: String) {
        if (closed) return
        closed = true
        AppLog.i("link closed: $reason")
        writer.shutdownNow()
        // Closing the fd makes a blocked read/write fail; the reader may
        // still sit in read() until the host sends something - it is a
        // daemon thread and ignores everything once `closed` is set.
        try { pfd?.close() } catch (_: IOException) { }
        main.post { listener.onClosed(this, reason) }
    }

    private fun write(frame: ByteArray) {
        if (closed) return
        writer.execute {
            try {
                output?.write(frame)
            } catch (e: IOException) {
                fail("write failed: ${e.message}")
            }
        }
    }

    private fun fail(reason: String) {
        main.post { close(reason) }
    }

    private fun readLoop(input: FileInputStream) {
        // Accessory reads need a buffer of at least 16 KB.
        val buf = ByteArray(16384)
        val dec = Proto.Decoder()
        try {
            while (!closed) {
                val n = input.read(buf)
                if (n < 0) {
                    fail("MMI closed the connection")
                    return
                }
                dec.feed(buf, 0, n, ::onFrame)
            }
        } catch (e: IOException) {
            if (!closed) fail("read failed: ${e.message}")
        }
    }

    // reader thread
    private fun onFrame(type: Int, payload: ByteArray) {
        if (closed) return
        when (type) {
            Proto.HELLO -> {
                write(Proto.encode(Proto.HELLO_REPLY, HELLO.toByteArray()))
                val peer = String(payload, Charsets.UTF_8)
                main.post { if (!closed) listener.onPeerHello(peer) }
            }
            Proto.HELLO_REPLY -> {
                val peer = String(payload, Charsets.UTF_8)
                main.post { if (!closed) listener.onPeerHello(peer) }
            }
            Proto.VIDEO_CAPS -> Proto.videoCapsOrNull(payload)?.let { caps ->
                if (videoCaps != caps) Log.i(TAG, "MMI video caps $caps")
                videoCaps = caps
            }
            Proto.VIDEO_ACKS -> Proto.videoAcksOrNull(payload)?.let(::onVideoAcks)
            Proto.H264_STATUS -> Proto.h264StatusOrNull(payload)?.let { code ->
                Log.i(TAG, "MMI H264_STATUS $code")
                when (code) {
                    Proto.H264_PLAYING -> h264.playing = true
                    Proto.H264_SYNC -> h264.sync.set(true)
                    Proto.H264_REFUSED -> h264.refused.set(true)
                    Proto.H264_CONFIG_WANTED -> h264.configWanted.set(true)
                }
            }
            else -> Log.w(TAG, "unknown frame type $type, ${payload.size} bytes")
        }
    }

    // reader thread
    private fun onVideoAcks(acks: Proto.VideoAcks) {
        video.shown += acks.shown
        video.bad += acks.bad
        video.dropped += acks.dropped
        inFlight.done(acks.total, SystemClock.elapsedRealtime())
    }

    companion object {
        private const val TAG = "AudiStreaming"
        const val ACK_TIMEOUT_MS = 3000L
        const val MAX_IN_FLIGHT = 2
        const val H264_IN_FLIGHT = 12      // ~0.5 s at 25 fps; acked every ~0.2 s
        val HELLO = "audi_streaming-android/1 ${Build.MANUFACTURER} ${Build.MODEL}"
    }
}
