package mmi3g.audistreaming

import android.app.Activity
import android.app.ActivityManager
import android.app.KeyguardManager
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
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
import android.os.PowerManager
import android.util.DisplayMetrics
import android.os.SystemClock
import android.view.Display
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Screen streaming to the MMI: the phone's screen (MediaProjection) as
 * JPEG pictures or H.264 ([Streaming.format]), pictures at most [Streaming.maxFps] a
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
 * Pictures are JPEG q90.
 * The pictures go to whatever link [Streaming.link] holds, so unplugging
 * and replugging the phone carries on streaming. Only a screen that
 * changes produces pictures; a new link gets the last one again.
 *
 * H.264 (the MMI's DM642 decodes it, docs/phone-mirroring.md
 * 7.10-7.12): the hardware encoder ([HwEncoder], 720x432 for the MMI's
 * 720x576 limit, Android 10+) gets the virtual display at k times that,
 * box-filtered k:1 on the GPU ([GlScaler]: straight into
 * the encoder, Android's one 3.25:1 bilinear step blurred text). The
 * "Sharp H.264" switch turns that off for comparison; a phone whose GL
 * setup fails gets the direct path. The bit rate is the drop-down's. The
 * encoder runs at the "Max fps" slider's rate (at most the MMI's 25) and
 * repeats the last frame when the screen is still; the MMI times its
 * stream at 25 fps and fills the rest with skip frames. Every frame goes
 * out in order - a lost one would spoil the ones after it - so frames
 * queue here while the MMI's acks are outstanding; if the queue gets long,
 * or a frame is bigger than the MMI's per-frame size, the frames up to the
 * next IDR are dropped and an IDR is asked for. A frame that doesn't fit
 * also raises the size (a new H264_CONFIG; the MMI restarts its stream).
 * The MMI's H264_STATUS: SYNC asks for an IDR, CONFIG for the config
 * again, REFUSED falls back to JPEG for this connection.
 *
 * [Streaming.keepScreenOn] holds a screen wake lock while streaming: the
 * activity's FLAG_KEEP_SCREEN_ON only counts while its window is visible,
 * and streaming is for while other apps are in front. The dim level is
 * enough to stop the timeout that would lock the phone, and dimming doesn't
 * touch the captured picture. The power button still turns the screen off.
 *
 * Diagnostics ([AppLog]): screen and lock events, the virtual display's
 * callbacks, how the projection ended, and every few seconds what the
 * capture produced. "display frames" count what the system composes into
 * the capture; "encoded" also counts the encoder's repeats of a still
 * picture, so encoded frames with no display frames mean a frozen picture.
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
    // the picture as a bitmap
    private var src = IntArray(0)
    private var px = IntArray(0)
    private var pxBuf: ByteBuffer? = null
    private var frame: Bitmap? = null
    private var nextId = 1
    private var last: Picture? = null
    private var lastSent = false
    private var lastLink: AccessoryLink? = null
    private lateinit var screenLock: PowerManager.WakeLock
    @Volatile private var destroying = false

    // what the last status line counted (capture thread)
    private var statusAt = 0L
    private var statusDisplayFrames = 0
    private var statusEncoded = 0
    private var statusDropped = 0

    private val displayEvents = object : VirtualDisplay.Callback() {
        override fun onPaused() = AppLog.i("virtual display paused: ${state()}")
        override fun onResumed() = AppLog.i("virtual display resumed: ${state()}")
        override fun onStopped() = AppLog.i("virtual display stopped: ${state()}")
    }

    private val screenEvents = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val reason = intent.getStringExtra("reason")?.let { " ($it)" } ?: ""
            val name = intent.action?.substringAfterLast('.')
            handler.post { AppLog.i("$name$reason: ${state()}") }
        }
    }

    // H.264 (capture thread only)
    private var enc: HwEncoder? = null
    private var scaler: GlScaler? = null              // null: the display draws into enc directly
    private var sps: ByteArray? = null
    private var pps: ByteArray? = null
    private var frameBytes = START_FRAME_BYTES
    private val queue = ArrayDeque<ByteArray>()
    private var waitIdr = true
    private var configLink: AccessoryLink? = null     // the link the config went to
    private var refusedLink: AccessoryLink? = null    // REFUSED: JPEG on this one
    private var h264Failed = ""                       // the phone can't: why
    private var h264ScalerFailed = ""                 // GL scaler failed once: direct from then on
    private var fpsChangeAt = 0L
    private var nextH264Id = 1
    private val h264 = H264Stats()

    private class H264Stats {
        var frames = 0; var bytes = 0L; var maxFrame = 0; var idrs = 0
        var dropped = 0; var tooBig = 0; var overflows = 0
        var windowAt = 0L; var windowBytes = 0L; var kbps = 0
    }

    private class Picture(val w: Int, val h: Int, val data: ByteArray, val phoneMs: Int)

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        AppLog.init(this)
        thread = HandlerThread("capture").apply { start() }
        handler = Handler(thread.looper)
        val filter = IntentFilter().apply {
            addAction(Intent.ACTION_SCREEN_OFF)
            addAction(Intent.ACTION_SCREEN_ON)
            addAction(Intent.ACTION_USER_PRESENT)
            @Suppress("DEPRECATION")
            addAction(Intent.ACTION_CLOSE_SYSTEM_DIALOGS)
        }
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(screenEvents, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(screenEvents, filter)
        }
        val power = getSystemService(Context.POWER_SERVICE) as PowerManager
        @Suppress("DEPRECATION")
        screenLock = power.newWakeLock(PowerManager.SCREEN_DIM_WAKE_LOCK, "audistreaming:keep_screen_on")
            .apply { setReferenceCounted(false) }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            AppLog.i("stop asked in the app or its notification")
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
            AppLog.w("no media projection (code $code)")
            stopSelf()
            return START_NOT_STICKY
        }
        projection = p
        // Registered before createVirtualDisplay (required since Android 14).
        p.registerCallback(object : MediaProjection.Callback() {
            override fun onStop() {
                AppLog.i("projection stopped ${if (destroying) "by the app" else "from outside the app"}: ${state()}")
                stopSelf()
            }
        }, handler)
        AppLog.i("projection started: ${state()}")
        Streaming.running = true
        Streaming.status = "starting"
        handler.post(::tick)
        return START_NOT_STICKY
    }

    override fun onTaskRemoved(rootIntent: Intent?) {
        AppLog.i("task removed from recents: ${state()}")
        super.onTaskRemoved(rootIntent)
    }

    override fun onTrimMemory(level: Int) {
        AppLog.i("trim memory, level $level")
        super.onTrimMemory(level)
    }

    override fun onDestroy() {
        destroying = true
        AppLog.i("service destroyed")
        unregisterReceiver(screenEvents)
        Streaming.running = false
        Streaming.status = ""
        handler.removeCallbacksAndMessages(null)
        handler.post {
            screenLock.release()
            display?.release()
            scaler?.release()
            scaler = null
            enc?.release()
            enc = null
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
     * unchanged, no MMI) look again shortly. H.264: every few ms. */
    private fun tick() {
        var next = POLL_MS
        if (Streaming.keepScreenOn != screenLock.isHeld) {
            if (Streaming.keepScreenOn) screenLock.acquire() else screenLock.release()
            AppLog.i("keep screen on: ${Streaming.keepScreenOn}")
        }
        logStatus()
        try {
            if (wantH264()) {
                next = h264Tick()
            } else {
                stopEncoder()
                if (capture()) next = 1000L / Streaming.maxFps.coerceIn(MIN_FPS, MAX_FPS)
            }
        } catch (e: RuntimeException) {
            AppLog.w("capture", e)
            Streaming.status = "capture error: ${e.message}"
        }
        if (projection != null) handler.postDelayed(::tick, next)
    }

    /** The phone's and the stream's state, for the log. */
    private fun state(): String {
        val power = getSystemService(Context.POWER_SERVICE) as PowerManager
        val keyguard = getSystemService(Context.KEYGUARD_SERVICE) as KeyguardManager
        val am = getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val process = ActivityManager.RunningAppProcessInfo()
        ActivityManager.getMyMemoryState(process)
        @Suppress("DEPRECATION")
        val foreground = am.getRunningServices(Int.MAX_VALUE).any {
            it.service.className == CaptureService::class.java.name && it.foreground
        }
        return "screen ${if (power.isInteractive) "on" else "off"}, " +
            "keyguard ${if (keyguard.isKeyguardLocked) "locked" else "open"}, " +
            "importance ${process.importance}, foreground service $foreground, " +
            "link ${if (Streaming.link != null) "up" else "down"}"
    }

    private fun logStatus() {
        val now = SystemClock.elapsedRealtime()
        if (now - statusAt < STATUS_LOG_MS) return
        statusAt = now
        val displayFrames = scaler?.frames ?: 0
        // a new scaler or encoder starts from 0 again
        val newDisplayFrames = displayFrames - if (displayFrames < statusDisplayFrames) 0 else statusDisplayFrames
        val v = Streaming.link?.video
        AppLog.i("status: ${state()}; " +
            (if (scaler != null) "display frames +$newDisplayFrames, " else "") +
            "encoded +${h264.frames - statusEncoded}, dropped +${h264.dropped - statusDropped}, queue ${queue.size}; " +
            "MMI sent ${v?.sent ?: 0}, shown ${v?.shown ?: 0}")
        statusDisplayFrames = displayFrames
        statusEncoded = h264.frames
        statusDropped = h264.dropped
    }

    // --- H.264 (capture thread) -------------------------------------------

    private fun wantH264(): Boolean {
        if (Streaming.format != Format.H264) return false
        val link = Streaming.link ?: return enc != null      // keep it through a replug
        val caps = link.videoCaps ?: return false
        return caps.h264 && link !== refusedLink && h264Failed.isEmpty() && Build.VERSION.SDK_INT >= 29
    }

    /** One step of the H.264 stream; returns the delay to the next. */
    private fun h264Tick(): Long {
        val link = Streaming.link
        if (link == null) {
            Streaming.status = "H.264: waiting for the MMI"
            return POLL_MS
        }
        val caps = link.videoCaps ?: return POLL_MS
        val wantFps = Streaming.maxFps.coerceIn(MIN_FPS, caps.h264Fps)
        val now = SystemClock.elapsedRealtime()
        val e = enc
        // a new rate on the slider, bit rate or scaling: a new encoder once
        // it has settled
        if (e != null && (e.fps != wantFps || e.bitRate != Streaming.bitRate ||
                (scaler != null) != (Streaming.sharp && h264ScalerFailed.isEmpty()))) {
            if (fpsChangeAt == 0L) fpsChangeAt = now
            if (now - fpsChangeAt >= FPS_SETTLE_MS) stopEncoder()
        } else {
            fpsChangeAt = 0L
        }
        scaler?.let {
            if (it.failed) {
                h264ScalerFailed = it.error
                AppLog.w("GL scaler stopped (${it.error}): direct from now on")
                stopEncoder()
            }
        }
        if (enc == null && !startEncoder(caps, wantFps)) return POLL_MS
        val encoder = enc ?: return POLL_MS

        if (link.h264.refused.getAndSet(false)) {
            refusedLink = link
            stopEncoder()
            Streaming.status = "the MMI refused H.264 (see its log): JPEG instead"
            return POLL_MS
        }
        if (link !== configLink && sps != null) sendConfig(link, "new connection")
        if (link.h264.configWanted.getAndSet(false)) sendConfig(link, "the MMI asked")
        if (link.h264.sync.getAndSet(false)) encoder.requestSync()
        pump(link)

        if (now - h264.windowAt >= 1000) {
            h264.kbps = ((h264.bytes - h264.windowBytes) * 8 / maxOf(1L, now - h264.windowAt)).toInt()
            h264.windowAt = now
            h264.windowBytes = h264.bytes
        }
        val sc = scaler
        Streaming.status = "H.264 ${encoder.w}x${encoder.h} " +
            (if (sc != null) "(x${sc.k}) " else "(direct${if (h264ScalerFailed.isNotEmpty()) ": GL failed" else ""}) ") +
            "${encoder.fps} fps, ${h264.kbps} of ${encoder.bitRate / 1000} kbit/s, " +
            "frames ${h264.frames} (IDR ${h264.idrs}), max ${h264.maxFrame / 1024} of ${frameBytes / 1024} KB" +
            (if (queue.isNotEmpty()) ", queue ${queue.size}" else "") +
            (if (h264.dropped > 0) ", dropped ${h264.dropped} (too big ${h264.tooBig}, " +
                "queue full ${h264.overflows})" else "") +
            if (link.h264.playing) "" else "; MMI starting"
        return H264_TICK_MS
    }

    private fun startEncoder(caps: Proto.VideoCaps, fps: Int): Boolean {
        val p = projection ?: return false
        val (w, h) = H264.pictureSize(caps.h264MaxW, caps.h264MaxH)
        val e = HwEncoder(w, h, fps, Streaming.bitRate, handler, ::onH264Config, ::onH264Frame) { why ->
            AppLog.w("H.264: $why")
            h264Failed = why
            Streaming.status = "H.264 failed ($why): JPEG instead"
        }
        if (!e.start()) return false
        // the one virtual display (Android 14 allows one per projection)
        // now draws into the encoder; the picture path gets it back on
        // its next resize
        reader?.close()
        reader = null
        size = null
        // the display at k x the picture, box-filtered k:1 on the GPU
        // (GlScaler); without it, Android's one bilinear step blurs text
        var input = e.surface
        var vw = w
        var vh = h
        if (Streaming.sharp && h264ScalerFailed.isEmpty()) {
            val screen = screenSize()
            val k = GlBox.factor(screen.w, screen.h, w, h)
            val sc = GlScaler(e.surface!!, w, h, k)
            if (sc.start()) {
                scaler = sc
                input = sc.inputSurface
                vw = sc.srcW
                vh = sc.srcH
            } else {
                h264ScalerFailed = sc.error
                AppLog.w("GL scaler failed (${sc.error}): the display draws into the encoder directly")
            }
        }
        val dpi = resources.displayMetrics.densityDpi
        val d = display
        if (d == null) {
            display = p.createVirtualDisplay("mmi-mirror", vw, vh, dpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, input, displayEvents, handler)
        } else {
            d.surface = input
            d.resize(vw, vh, dpi)
        }
        enc = e
        sps = null
        pps = null
        configLink = null
        queue.clear()
        waitIdr = true
        // the size the frames needed stays: from 64 KB again, the next big
        // IDR would cost one more restart on the MMI
        frameBytes = frameBytes.coerceAtMost(caps.h264MaxFrame)
        AppLog.i("H.264 on: ${e.description}, display ${vw}x$vh${scaler?.let { " -> GL x${it.k}" } ?: " direct"}")
        return true
    }

    private fun stopEncoder() {
        val e = enc ?: return
        display?.surface = null
        scaler?.release()                 // nothing draws into the encoder after this
        scaler = null
        e.release()
        enc = null
        sps = null
        pps = null
        configLink = null
        queue.clear()
        AppLog.i("H.264 off")
    }

    private fun sendConfig(link: AccessoryLink, why: String) {
        val s = sps ?: return
        val p = pps ?: return
        val caps = link.videoCaps ?: return
        link.sendH264Config(Proto.h264Config(caps.h264Fps, frameBytes, s, p))
        configLink = link
        // the MMI (re)starts its stream at the next IDR
        queue.clear()
        waitIdr = true
        enc?.requestSync()
        AppLog.i("H.264 config sent ($why): ${frameBytes / 1024} KB frames, SPS ${s.size} / PPS ${p.size} B")
    }

    // encoder callback (capture thread)
    private fun onH264Config(s: ByteArray, p: ByteArray) {
        sps = s
        pps = p
        Streaming.link?.let { sendConfig(it, "encoder started") }
    }

    // encoder callback (capture thread)
    private fun onH264Frame(data: ByteArray, key: Boolean) {
        val link = Streaming.link
        if (link == null || configLink !== link) return      // the MMI has no stream for it
        val idr = key && H264.isIdr(data)
        if (waitIdr && !idr) {
            h264.dropped++
            return
        }
        // an encoder that repeats SPS/PPS before its IDRs, but not the
        // csd's bytes: the MMI refuses in-band ones that differ from its
        // config (and would ask for the same config again forever), so the
        // in-band ones become the config
        if (idr) {
            val inBand = H264.spsPps(data)
            val s = sps
            val p = pps
            if (inBand != null && s != null && p != null &&
                (!inBand.first.contentEquals(s) || !inBand.second.contentEquals(p))) {
                sps = inBand.first
                pps = inBand.second
                h264.dropped++
                sendConfig(link, "in-band SPS/PPS differ from the codec config")
                return
            }
        }
        val need = H264.sampleBytes(data)
        if (need > frameBytes) {
            h264.dropped++
            h264.tooBig++
            val max = link.videoCaps?.h264MaxFrame ?: frameBytes
            val grown = H264.grow(frameBytes, need, max)
            AppLog.w("H.264 frame of $need B > $frameBytes B${if (grown > frameBytes) ": now $grown" else ""}")
            if (grown > frameBytes) {
                frameBytes = grown
                sendConfig(link, "bigger frames")
            } else {
                waitIdr = true
                enc?.requestSync()
            }
            return
        }
        if (queue.size >= MAX_QUEUE) {
            h264.dropped += queue.size + 1
            h264.overflows++
            queue.clear()
            waitIdr = true
            enc?.requestSync()
            return
        }
        waitIdr = false
        queue.addLast(data)
        h264.frames++
        h264.bytes += data.size
        if (idr) h264.idrs++
        if (need > h264.maxFrame) h264.maxFrame = need
        pump(link)
    }

    private fun pump(link: AccessoryLink) {
        val e = enc ?: return
        while (queue.isNotEmpty() && link.readyForPicture(AccessoryLink.H264_IN_FLIGHT)) {
            val f = queue.first()
            if (!link.sendPicture(nextH264Id, e.w, e.h, Proto.FMT_H264, f, 0, AccessoryLink.H264_IN_FLIGHT)) break
            queue.removeFirst()
            nextH264Id = if (nextH264Id == 0xffff) 1 else nextH264Id + 1
        }
    }

    // --- pictures (capture thread) ------------------------------------------

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

        val note = if (Streaming.format == Format.H264) {
            "H.264 off: " + when {
                Build.VERSION.SDK_INT < 29 -> "needs Android 10"
                h264Failed.isNotEmpty() -> h264Failed
                link === refusedLink -> "the MMI refused it"
                else -> "the MMI app has none (older audi_stream_receiver?)"
            } + ", JPEG instead"
        } else ""
        val pic = r.acquireLatestImage()?.use { image ->
            val t0 = SystemClock.elapsedRealtime()
            grab(image)
            encode(t0)
        }
        // A static screen makes no new images: a new link (replug) gets the
        // last picture again, otherwise the MMI keeps the one it shows.
        val toSend = pic ?: last?.takeIf { link !== lastLink || !lastSent }
            ?: return false
        last = toSend
        lastSent = false
        if (toSend.data.size > caps.maxBytes) {
            Streaming.status = "picture ${toSend.data.size} B > MMI max ${caps.maxBytes} B"
            lastSent = true                  // never fits: don't retry it
            return false
        }
        if (!link.sendPicture(nextId, toSend.w, toSend.h, Proto.FMT_JPEG, toSend.data, toSend.phoneMs)) return false
        nextId = if (nextId == 0xffff) 1 else nextId + 1
        lastLink = link
        lastSent = true
        Streaming.status = "${toSend.w}x${toSend.h} (x$factor) JPEG " +
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

    /** [frame] as JPEG; phoneMs counts from t0 (capture taken). */
    private fun encode(t0: Long): Picture {
        val f = frame!!
        val out = ByteArrayOutputStream(64 * 1024)
        f.compress(Bitmap.CompressFormat.JPEG, QUALITY, out)
        return Picture(f.width, f.height, out.toByteArray(), (SystemClock.elapsedRealtime() - t0).toInt())
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
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, r.surface, displayEvents, handler)
        } else {
            d.surface = r.surface
            d.resize(cw, ch, dpi)
        }
        reader = r
        old?.close()
        px = IntArray(s.w * s.h)
        pxBuf = ByteBuffer.allocateDirect(s.w * s.h * 4).order(ByteOrder.nativeOrder())
        frame?.recycle()
        frame = Bitmap.createBitmap(s.w, s.h, Bitmap.Config.ARGB_8888).apply { setHasAlpha(false) }
        AppLog.i("capture ${cw}x${ch} -> ${s.w}x${s.h} (${if (s.w < s.h) "portrait" else "landscape"}, x$k)")
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
        return b.setSmallIcon(R.drawable.ic_stat_stream)
            .setContentTitle("Streaming the screen to the MMI")
            .setContentIntent(open)
            .addAction(Notification.Action.Builder(null, "Stop", stop).build())
            .setOngoing(true)
            .build()
    }

    companion object {
        private const val STATUS_LOG_MS = 5000L
        private const val CHANNEL = "capture"
        private const val NOTIFICATION_ID = 1
        private const val ACTION_STOP = "mmi3g.audistreaming.STOP_CAPTURE"
        private const val EXTRA_CODE = "code"
        private const val EXTRA_DATA = "data"
        const val DEFAULT_FPS = 5
        const val MIN_FPS = 1
        const val MAX_FPS = 25            // H.264 runs at up to the MMI's 25
        private const val POLL_MS = 20L   // waiting for an ack or a new image
        const val QUALITY = 90            // 60 rings around text
        private const val H264_TICK_MS = 5L
        private const val FPS_SETTLE_MS = 700L
        // H.264 bit rates of the drop-down (res/values/strings.xml bit_rates);
        // 2.5 Mbit/s is ~12 KB a frame at 25 fps
        val BIT_RATES = intArrayOf(2_500_000, 4_000_000, 6_000_000, 8_000_000)
        const val DEFAULT_BIT_RATE = 4_000_000
        const val START_FRAME_BYTES = 64 * 1024
        private const val MAX_QUEUE = 25  // ~1 s at 25 fps not acked: start again at an IDR

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
    @Volatile var format = Format.H264
    /** H.264 bits a second, set by the activity (its drop-down). */
    @Volatile var bitRate = CaptureService.DEFAULT_BIT_RATE
    /** H.264 through the GL scaler (sharp text) or straight from the
     * display (the old way, for comparison), set by the activity. */
    @Volatile var sharp = true
    /** Hold the screen on while streaming, set by the activity (its checkbox). */
    @Volatile var keepScreenOn = false
}

/** The format drop-down, in its order (res/values/strings.xml formats). A
 * saved choice that is no longer here reads as H.264. */
enum class Format {
    /** The phone's hardware encoder, decoded by the MMI's DM642. */
    H264,
    JPEG,
}
