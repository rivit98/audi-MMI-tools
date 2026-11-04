package mmi3g.audistreaming

import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.Bitmap
import android.graphics.PixelFormat
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.Image
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.util.DisplayMetrics
import android.util.Log
import android.os.SystemClock
import android.view.Display
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Screen streaming to the MMI: the phone's screen (MediaProjection) as
 * JPEG or PNG pictures ([Streaming.format]), at most [Streaming.maxFps] a
 * second and never more than
 * AccessoryLink.MAX_IN_FLIGHT in flight. The next picture goes as soon as
 * one in flight is acked and 1/maxFps has passed since the last was sent,
 * so a slow MMI sets the rate itself (a fixed tick would skip every other
 * one when an ack takes a little longer than the interval). Runs as a
 * foreground service so it keeps going while another app is in front -
 * that is the point of it.
 *
 * The capture is a virtual display mirroring the screen at k times the
 * picture that fits the MMI's box for the current orientation
 * (VideoFit.fit / factor; k = 2 landscape, 4 portrait on a 2340x1080
 * phone), box-filtered down k:1 (Downscale) - one big bilinear step made
 * text blurry. A rotation resizes it.
 *
 * Formats: JPEG (q90), PNG (lossless; one too big for the MMI goes as
 * JPEG), or PNG when still: JPEG while the screen changes, then the same
 * picture again as PNG once it has not changed for [STILL_MS]. PNG needs
 * an MMI app that takes it (VIDEO_CAPS); an older one gets JPEG.
 * The pictures go to whatever link [Streaming.link] holds, so unplugging
 * and replugging the phone carries on streaming. Only a screen that
 * changes produces pictures; a new link gets the last one again.
 */
class CaptureService : Service() {
    private lateinit var thread: HandlerThread
    private lateinit var handler: Handler
    private var projection: MediaProjection? = null
    private var display: VirtualDisplay? = null
    private var reader: ImageReader? = null
    private var size: VideoFit.Size? = null
    private var factor = 1
    // capture -> picture: the capture's pixels, the downscaled ones, and
    // the picture as a bitmap (the last one captured, for a still PNG)
    private var src = IntArray(0)
    private var px = IntArray(0)
    private var pxBuf: ByteBuffer? = null
    private var frame: Bitmap? = null
    private var frameAt = 0L            // elapsedRealtime of the frame's capture, 0 = none
    private var stillDone = true        // the frame went as PNG already (or needs not)
    private var nextId = 1
    private var last: Picture? = null
    private var lastSent = false
    private var lastLink: AccessoryLink? = null

    private class Picture(val w: Int, val h: Int, val format: Int, val data: ByteArray, val phoneMs: Int)

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        thread = HandlerThread("capture").apply { start() }
        handler = Handler(thread.looper)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        if (projection != null) return START_NOT_STICKY
        val code = intent?.getIntExtra(EXTRA_CODE, Activity.RESULT_CANCELED) ?: Activity.RESULT_CANCELED
        @Suppress("DEPRECATION")
        val data: Intent? = intent?.getParcelableExtra(EXTRA_DATA)
        // Foreground (type mediaProjection) before getMediaProjection, or
        // Android 14 refuses it.
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTIFICATION_ID, notification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
        } else {
            startForeground(NOTIFICATION_ID, notification())
        }
        val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        val p = if (code == Activity.RESULT_OK && data != null) mpm.getMediaProjection(code, data) else null
        if (p == null) {
            Log.w(TAG, "no media projection (code $code)")
            stopSelf()
            return START_NOT_STICKY
        }
        projection = p
        // Registered before createVirtualDisplay (required since Android 14).
        p.registerCallback(object : MediaProjection.Callback() {
            override fun onStop() {
                Log.i(TAG, "projection stopped")
                stopSelf()
            }
        }, handler)
        Streaming.running = true
        Streaming.status = "starting"
        handler.post(::tick)
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        Streaming.running = false
        Streaming.status = ""
        handler.removeCallbacksAndMessages(null)
        handler.post {
            display?.release()
            reader?.close()
            frame?.recycle()
            projection?.stop()
            display = null
            reader = null
            projection = null
            Streaming.link?.sendVideoStop()
            thread.quitSafely()
        }
        super.onDestroy()
    }

    // --- capture thread ----------------------------------------------------

    /** Sent a picture: wait out the fps limit. Else (MMI has its fill, screen
     * unchanged, no MMI) look again shortly. */
    private fun tick() {
        var next = POLL_MS
        try {
            if (capture()) next = 1000L / Streaming.maxFps.coerceIn(MIN_FPS, MAX_FPS)
        } catch (e: RuntimeException) {
            Log.w(TAG, "capture", e)
            Streaming.status = "capture error: ${e.message}"
        }
        if (projection != null) handler.postDelayed(::tick, next)
    }

    /** True if a picture was sent. */
    private fun capture(): Boolean {
        val link = Streaming.link
        val caps = link?.videoCaps
        if (link == null || caps == null || !caps.jpeg) {
            Streaming.status = when {
                link == null -> "waiting for the MMI"
                caps == null -> "MMI app has no video (old audi_stream_receiver?)"
                else -> "MMI takes no JPEG"
            }
            return false
        }
        val screen = screenSize()
        val want = VideoFit.fit(screen.w, screen.h, caps)
        if (want != size) resize(want, VideoFit.factor(screen.w, screen.h, want))
        val r = reader ?: return false
        // Not while the MMI has its fill: an image taken now could not be
        // sent, and a static screen makes no new one to replace it.
        if (!link.readyForPicture()) return false

        val mode = Streaming.format
        val png = caps.png
        var note = if (mode != Format.JPEG && !png) "MMI takes no PNG (old audi_stream_receiver?)" else ""
        var pic = r.acquireLatestImage()?.use { image ->
            val t0 = SystemClock.elapsedRealtime()
            grab(image)
            frameAt = t0
            stillDone = mode != Format.PNG_STILL || !png
            if (mode == Format.PNG && png) {
                encode(Proto.FMT_PNG, t0).takeIf { it.data.size <= caps.maxBytes }
                    ?: encode(Proto.FMT_JPEG, t0).also { note = "PNG too big, sent JPEG" }
            } else {
                encode(Proto.FMT_JPEG, t0)
            }
        }
        if (pic == null && mode == Format.PNG_STILL && !stillDone &&
            SystemClock.elapsedRealtime() - frameAt >= STILL_MS) {
            stillDone = true
            pic = encode(Proto.FMT_PNG, SystemClock.elapsedRealtime())
            if (pic.data.size > caps.maxBytes) {
                Streaming.status = "still PNG ${pic.data.size / 1024} KB too big, JPEG stays"
                return false
            }
        }
        // A static screen makes no new images: a new link (replug) gets the
        // last picture again, otherwise the MMI keeps the one it shows.
        val toSend = pic ?: last?.takeIf { (link !== lastLink || !lastSent) && caps.takes(it.format) }
            ?: return false
        last = toSend
        lastSent = false
        if (toSend.data.size > caps.maxBytes) {
            Streaming.status = "picture ${toSend.data.size} B > MMI max ${caps.maxBytes} B"
            lastSent = true                  // never fits: don't retry it
            return false
        }
        if (!link.sendPicture(nextId, toSend.w, toSend.h, toSend.format, toSend.data, toSend.phoneMs)) return false
        nextId = if (nextId == 0xffff) 1 else nextId + 1
        lastLink = link
        lastSent = true
        Streaming.status = "${toSend.w}x${toSend.h} (x$factor) ${if (toSend.format == Proto.FMT_PNG) "PNG" else "JPEG"} " +
            "${toSend.data.size / 1024} KB, phone ${toSend.phoneMs} ms" + if (note.isNotEmpty()) "; $note" else ""
        return true
    }

    /** The image, box-filtered down [factor]:1, into [frame]. */
    private fun grab(image: Image) {
        val s = size ?: return
        val plane = image.planes[0]
        val stride = plane.rowStride / plane.pixelStride
        val ints = plane.buffer.order(ByteOrder.nativeOrder()).asIntBuffer()
        // the last row may come without its padding
        val n = minOf(ints.remaining(), stride * image.height)
        if (src.size < stride * image.height) src = IntArray(stride * image.height)
        ints.get(src, 0, n)
        Downscale.box(src, stride, factor, s.w, s.h, px)
        val buf = pxBuf!!
        buf.clear()
        buf.asIntBuffer().put(px, 0, s.w * s.h)
        frame!!.copyPixelsFromBuffer(buf)
    }

    /** [frame] as format; phoneMs counts from t0 (capture taken). */
    private fun encode(format: Int, t0: Long): Picture {
        val f = frame!!
        val out = ByteArrayOutputStream(if (format == Proto.FMT_PNG) 256 * 1024 else 64 * 1024)
        if (format == Proto.FMT_PNG) {
            f.compress(Bitmap.CompressFormat.PNG, 100, out)
        } else {
            f.compress(Bitmap.CompressFormat.JPEG, QUALITY, out)
        }
        return Picture(f.width, f.height, format, out.toByteArray(), (SystemClock.elapsedRealtime() - t0).toInt())
    }

    private fun screenSize(): VideoFit.Size {
        val dm = getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
        val metrics = DisplayMetrics()
        @Suppress("DEPRECATION")
        dm.getDisplay(Display.DEFAULT_DISPLAY).getRealMetrics(metrics)
        return VideoFit.Size(metrics.widthPixels, metrics.heightPixels)
    }

    /** First call creates the virtual display; later ones (rotation, other
     * MMI caps) resize it - Android 14 allows one per projection. The
     * capture is k times the picture s. */
    private fun resize(s: VideoFit.Size, k: Int) {
        val p = projection ?: return
        val dpi = resources.displayMetrics.densityDpi
        val cw = s.w * k
        val ch = s.h * k
        val old = reader
        val r = ImageReader.newInstance(cw, ch, PixelFormat.RGBA_8888, 2)
        val d = display
        if (d == null) {
            display = p.createVirtualDisplay("mmi-mirror", cw, ch, dpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, r.surface, null, handler)
        } else {
            d.surface = r.surface
            d.resize(cw, ch, dpi)
        }
        reader = r
        old?.close()
        px = IntArray(s.w * s.h)
        pxBuf = ByteBuffer.allocateDirect(s.w * s.h * 4).order(ByteOrder.nativeOrder())
        frame?.recycle()
        // opaque: PNG goes out as RGB, not RGBA
        frame = Bitmap.createBitmap(s.w, s.h, Bitmap.Config.ARGB_8888).apply { setHasAlpha(false) }
        frameAt = 0L
        stillDone = true
        Log.i(TAG, "capture ${cw}x${ch} -> ${s.w}x${s.h} (${if (s.w < s.h) "portrait" else "landscape"}, x$k)")
        size = s
        factor = k
    }

    private fun notification(): Notification {
        val stop = PendingIntent.getService(this, 0,
            Intent(this, CaptureService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE)
        val open = PendingIntent.getActivity(this, 0,
            Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val b = if (Build.VERSION.SDK_INT >= 26) {
            val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            nm.createNotificationChannel(NotificationChannel(CHANNEL, "Screen streaming",
                NotificationManager.IMPORTANCE_LOW))
            Notification.Builder(this, CHANNEL)
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(this)
        }
        @Suppress("DEPRECATION")
        return b.setSmallIcon(android.R.drawable.presence_video_online)
            .setContentTitle("Streaming the screen to the MMI")
            .setContentIntent(open)
            .addAction(Notification.Action.Builder(null, "Stop", stop).build())
            .setOngoing(true)
            .build()
    }

    companion object {
        private const val TAG = "AudiStreaming"
        private const val CHANNEL = "capture"
        private const val NOTIFICATION_ID = 1
        private const val ACTION_STOP = "mmi3g.audistreaming.STOP_CAPTURE"
        private const val EXTRA_CODE = "code"
        private const val EXTRA_DATA = "data"
        const val DEFAULT_FPS = 5
        const val MIN_FPS = 1
        const val MAX_FPS = 15
        private const val POLL_MS = 20L   // waiting for an ack or a new image
        const val QUALITY = 90            // 60 until 2026-10-30: ringing around text
        const val STILL_MS = 300L         // PNG when still: unchanged this long

        fun start(context: Context, resultCode: Int, data: Intent) {
            val i = Intent(context, CaptureService::class.java)
                .putExtra(EXTRA_CODE, resultCode)
                .putExtra(EXTRA_DATA, data)
            if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(i) else context.startService(i)
        }

        fun stop(context: Context) {
            context.startService(Intent(context, CaptureService::class.java).setAction(ACTION_STOP))
        }
    }
}

/** What the capture service and the activity share. */
object Streaming {
    /** The open link to the MMI, set by the activity. */
    @Volatile var link: AccessoryLink? = null
    @Volatile var running = false
    @Volatile var status = ""
    /** Pictures a second at most, set by the activity (its slider). */
    @Volatile var maxFps = CaptureService.DEFAULT_FPS
    /** Picture format, set by the activity (its drop-down). */
    @Volatile var format = Format.JPEG
}

/** The format drop-down, in its order (res/values/strings.xml formats). */
enum class Format {
    JPEG,
    PNG,
    /** JPEG while the screen changes, PNG once it has stopped. */
    PNG_STILL,
}
