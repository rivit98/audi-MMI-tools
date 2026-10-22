package mmi3g.usbtx

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
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.util.DisplayMetrics
import android.util.Log
import android.view.Display
import java.io.ByteArrayOutputStream

/**
 * Screen streaming to the MMI: the phone's screen (MediaProjection) as
 * JPEG pictures, at most every [INTERVAL_MS] and never more than one in
 * flight (AccessoryLink.sendPicture). Runs as a foreground service so it
 * keeps going while another app is in front - that is the point of it.
 *
 * The capture is a virtual display mirroring the screen, sized to fit the
 * MMI's box for the current orientation (VideoFit); a rotation resizes it.
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
    private var padded: Bitmap? = null
    private var nextId = 1
    private var last: Picture? = null
    private var lastLink: AccessoryLink? = null

    private class Picture(val w: Int, val h: Int, val jpeg: ByteArray)

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

    private fun tick() {
        try {
            capture()
        } catch (e: RuntimeException) {
            Log.w(TAG, "capture", e)
            Streaming.status = "capture error: ${e.message}"
        }
        if (projection != null) handler.postDelayed(::tick, INTERVAL_MS)
    }

    private fun capture() {
        val link = Streaming.link
        val caps = link?.videoCaps
        if (link == null || caps == null || !caps.jpeg) {
            Streaming.status = when {
                link == null -> "waiting for the MMI"
                caps == null -> "MMI app has no video (old usb_tx_test?)"
                else -> "MMI takes no JPEG"
            }
            return
        }
        val screen = screenSize()
        val want = VideoFit.fit(screen.w, screen.h, caps)
        if (want != size) resize(want)
        val r = reader ?: return

        val pic = r.acquireLatestImage()?.use { image ->
            val plane = image.planes[0]
            val padW = plane.rowStride / plane.pixelStride
            var bmp = padded
            if (bmp == null || bmp.width != padW || bmp.height != image.height) {
                bmp = Bitmap.createBitmap(padW, image.height, Bitmap.Config.ARGB_8888)
                padded = bmp
            }
            bmp.copyPixelsFromBuffer(plane.buffer)
            val crop = if (padW == image.width) bmp else Bitmap.createBitmap(bmp, 0, 0, image.width, image.height)
            val out = ByteArrayOutputStream(64 * 1024)
            crop.compress(Bitmap.CompressFormat.JPEG, QUALITY, out)
            if (crop !== bmp) crop.recycle()
            Picture(image.width, image.height, out.toByteArray())
        }
        // A static screen makes no new images: a new link (replug) gets the
        // last picture again, otherwise the MMI keeps the one it shows.
        val toSend = pic ?: last.takeIf { link !== lastLink } ?: return
        if (toSend.jpeg.size > caps.maxBytes) {
            Streaming.status = "picture ${toSend.jpeg.size} B > MMI max ${caps.maxBytes} B"
            return
        }
        if (link.sendPicture(nextId, toSend.w, toSend.h, toSend.jpeg)) {
            nextId = if (nextId == 0xffff) 1 else nextId + 1
            lastLink = link
            Streaming.status = "${toSend.w}x${toSend.h}, ${toSend.jpeg.size / 1024} KB"
        }
        last = toSend
    }

    private fun screenSize(): VideoFit.Size {
        val dm = getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
        val metrics = DisplayMetrics()
        @Suppress("DEPRECATION")
        dm.getDisplay(Display.DEFAULT_DISPLAY).getRealMetrics(metrics)
        return VideoFit.Size(metrics.widthPixels, metrics.heightPixels)
    }

    /** First call creates the virtual display; later ones (rotation, other
     * MMI caps) resize it - Android 14 allows one per projection. */
    private fun resize(s: VideoFit.Size) {
        val p = projection ?: return
        val dpi = resources.displayMetrics.densityDpi
        val old = reader
        val r = ImageReader.newInstance(s.w, s.h, PixelFormat.RGBA_8888, 2)
        val d = display
        if (d == null) {
            display = p.createVirtualDisplay("mmi-mirror", s.w, s.h, dpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, r.surface, null, handler)
        } else {
            d.surface = r.surface
            d.resize(s.w, s.h, dpi)
        }
        reader = r
        old?.close()
        Log.i(TAG, "capture ${s.w}x${s.h} (${if (s.w < s.h) "portrait" else "landscape"})")
        size = s
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
        private const val TAG = "usb_tx_test"
        private const val CHANNEL = "capture"
        private const val NOTIFICATION_ID = 1
        private const val ACTION_STOP = "mmi3g.usbtx.STOP_CAPTURE"
        private const val EXTRA_CODE = "code"
        private const val EXTRA_DATA = "data"
        const val INTERVAL_MS = 500L       // 2 pictures a second at most
        const val QUALITY = 60

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
}
