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
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
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
import android.view.WindowInsets
import android.view.WindowManager

/**
 * Screen streaming to the MMI: the phone's screen (MediaProjection) as
 * H.264, which the MMI's DM642 decodes (docs/phone-mirroring.md
 * 7.10-7.12). Runs as a foreground service so it keeps going while another
 * app is in front - that is the point of it.
 *
 * The hardware encoder ([HwEncoder], 720x432 for the MMI's 720x576 limit,
 * Android 10+) gets the virtual display through [GlScaler], which leaves the
 * navigation bar out ([ScreenCrop], unless [Streaming.hideNavBar] is off)
 * and box-filters k:1 (straight into the encoder, Android's one 3.25:1
 * bilinear step blurred text). The "Sharp H.264" switch makes k = 1 for
 * comparison. A phone whose GL setup fails gets the direct path, navigation
 * bar included. The bit rate is the drop-down's. The scaler draws at
 * exactly the "Max fps" slider's rate (at most the MMI's 25, [FrameClock]),
 * a still screen too; the MMI times its stream at 25 fps and fills the rest
 * with skip frames. The frames go to the MMI through [MmiH264] (queue, IDRs,
 * the MMI's H264_STATUS), on whatever link [Streaming.link] holds, so
 * unplugging and replugging the phone carries on streaming.
 *
 * H.264 is the only format. A REFUSED pauses the stream on that link until
 * H.264 is tried again ([RefusedBackoff]: 5 s, doubling to 60 s). An MMI app
 * without H.264 gets nothing, and a phone that can't encode H.264 (before
 * Android 10, or the encoder fails) stops streaming; [Streaming.status]
 * says why.
 *
 * H.264 is landscape only: a portrait screen would fill a narrow strip of
 * the MMI with unreadable text, so while the phone is portrait the scaler
 * draws a "turn the phone to landscape" card ([Notice]) instead, into the
 * same stream (no MMI restart). The direct path can't draw it: its picture
 * stops (the MMI holds the last one).
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
    // the screen and what to leave out of it, read every SCREEN_CHECK_MS
    private var screen = VideoFit.Size(0, 0)
    private var insets = ScreenCrop.NONE
    private var screenAt = 0L
    private lateinit var screenLock: PowerManager.WakeLock
    @Volatile private var destroying = false
    // why streaming stopped by itself (H.264 can't run on this phone): kept
    // in Streaming.status after the service is gone
    @Volatile private var stopReason = ""

    // what the last status line counted (capture thread)
    private var statusAt = 0L
    private var settingsLink: AccessoryLink? = null   // the link the settings went to
    private var settingsSent = ""                     // and what they were
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
    private val mmi = MmiH264()                       // the encoder's frames to the MMI
    private val enc: HwEncoder? get() = mmi.enc
    private var scaler: GlScaler? = null              // null: the display draws into enc directly
    private var h264ScalerFailed = ""                 // GL scaler failed once: direct from then on
    private var fpsChangeAt = 0L
    private var portrait = false                      // the notice is up instead of the screen

    // the phone's playback to the MMI as SBC ([PhoneAudio]), while
    // Streaming.sendAudio is on (capture thread)
    private var audio: PhoneAudio? = null
    private var audioRefused = false                  // said once: no permission / too old

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
        Streaming.status = stopReason
        handler.removeCallbacksAndMessages(null)
        handler.post {
            screenLock.release()
            audio?.stop()
            audio = null
            display?.release()
            scaler?.release()
            scaler = null
            enc?.release()
            mmi.encoderGone()
            projection?.stop()
            display = null
            projection = null
            Streaming.link?.sendVideoStop()
            thread.quitSafely()
        }
        super.onDestroy()
    }

    // --- capture thread ----------------------------------------------------

    /** One step of streaming, every few ms. */
    private fun tick() {
        var next = POLL_MS
        if (Streaming.keepScreenOn != screenLock.isHeld) {
            if (Streaming.keepScreenOn) screenLock.acquire() else screenLock.release()
            AppLog.i("keep screen on: ${Streaming.keepScreenOn}")
        }
        logStatus()
        followAudio()
        try {
            sendSettings()
            next = h264Tick()
        } catch (e: RuntimeException) {
            AppLog.w("capture", e)
            Streaming.status = "capture error: ${e.message}"
        }
        if (projection != null) handler.postDelayed(::tick, next)
    }

    /** The phone's audio capture on or off with [Streaming.sendAudio]. */
    private fun followAudio() {
        val p = projection ?: return
        if (!Streaming.sendAudio) {
            if (audio != null) AppLog.i("audio: switched off")
            audio?.stop()
            audio = null
            return
        }
        if (audio != null) return
        val allowed = Build.VERSION.SDK_INT >= 29 &&
            checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) ==
            android.content.pm.PackageManager.PERMISSION_GRANTED
        if (!allowed) {
            if (!audioRefused) AppLog.i("audio: off (${if (Build.VERSION.SDK_INT < 29) "needs Android 10" else "no RECORD_AUDIO permission"})")
            audioRefused = true
            return
        }
        audio = PhoneAudio(p).also { it.start() }
    }

    /** What streaming runs with, for the MMI's log (STREAM_SETTINGS) and ours:
     * the user's settings and the frame size. */
    private fun settingsText(): String {
        fun on(b: Boolean) = if (b) 1 else 0
        return "max_fps=${Streaming.maxFps} bit_rate_kbit=${Streaming.bitRate / 1000} " +
            "sharp=${on(Streaming.sharp)} " +
            "hide_nav_bar=${on(Streaming.hideNavBar)} keep_screen_on=${on(Streaming.keepScreenOn)} " +
            "audio=${on(Streaming.sendAudio)} " +
            "frame_kb=${mmi.frameBytes / 1024} android=${Build.VERSION.SDK_INT}"
    }

    /** STREAM_SETTINGS to the link when streaming starts on it or a setting
     * changes; up= (ms since the phone booted) lines its log up with ours. */
    private fun sendSettings() {
        val link = Streaming.link ?: return
        if (link.videoCaps == null) return                    // no MMI app yet
        val text = settingsText()
        if (link === settingsLink && text == settingsSent) return
        settingsLink = link
        settingsSent = text
        val line = "up=${SystemClock.elapsedRealtime()} $text"
        link.sendSettings(line)
        AppLog.i("settings sent: $line")
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
            (if (scaler != null) "display frames +$newDisplayFrames, drawn ${scaler?.drawn ?: 0}, " else "") +
            "encoded +${mmi.stats.frames - statusEncoded}, dropped +${mmi.stats.dropped - statusDropped}, queue ${mmi.queued}; " +
            "MMI sent ${v?.sent ?: 0}, shown ${v?.shown ?: 0}")
        statusDisplayFrames = displayFrames
        statusEncoded = mmi.stats.frames
        statusDropped = mmi.stats.dropped
    }

    // --- H.264 (capture thread) -------------------------------------------

    /** One step of the H.264 stream; returns the delay to the next. */
    private fun h264Tick(): Long {
        if (stopReason.isNotEmpty()) return POLL_MS          // stopping
        if (!phoneCanH264) {
            fail("needs Android 10 or newer")
            return POLL_MS
        }
        // no link: the encoder stays, for a replug
        val link = Streaming.link
        if (link == null) {
            Streaming.status = "H.264: waiting for the MMI"
            return POLL_MS
        }
        val caps = link.videoCaps
        if (caps == null || !caps.h264) {
            stopEncoder()
            Streaming.status = if (caps == null) "waiting for the MMI app's video caps"
                else "the MMI app has no H.264 (older audi_stream_receiver?): nothing to send"
            return POLL_MS
        }
        if (mmi.refused(link)) {
            stopEncoder()
            Streaming.status = "the MMI refused H.264 (see its log), trying again soon"
            return POLL_MS
        }
        val wantFps = Streaming.maxFps.coerceIn(MIN_FPS, caps.h264Fps)
        val now = SystemClock.elapsedRealtime()
        val e = enc
        // a new rate on the slider or bit rate: a new encoder once it has
        // settled
        if (e != null && (e.fps != wantFps || e.bitRate != Streaming.bitRate)) {
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

        followScreen()
        followOrientation(encoder)
        // the MMI's status, then the frames
        if (!mmi.step(link)) {
            stopEncoder()
            Streaming.status = "the MMI refused H.264 (see its log), trying again soon"
            return POLL_MS
        }
        val sc = scaler
        Streaming.status = "H.264 ${encoder.w}x${encoder.h} " +
            (if (sc != null) "(x${sc.k}) " else "(direct${if (h264ScalerFailed.isNotEmpty()) ": GL failed" else ""}) ") +
            mmi.describe(encoder) +
            (if (portrait) "; portrait: the MMI shows \"turn the phone to landscape\"" else "") +
            if (link.h264.playing) "" else "; MMI starting"
        return H264_TICK_MS
    }

    private fun startEncoder(caps: Proto.VideoCaps, fps: Int): Boolean {
        val p = projection ?: return false
        val (w, h) = H264.pictureSize(caps.h264MaxW, caps.h264MaxH)
        // behind the GL scaler the still screen is drawn on its clock: no
        // encoder repeats on top of that
        val viaGl = h264ScalerFailed.isEmpty()
        val e = HwEncoder(w, h, fps, Streaming.bitRate, !viaGl, handler, mmi::onConfig, mmi::onFrame, ::fail)
        if (!e.start()) return false
        // the one virtual display (Android 14 allows one per projection)
        // draws into the encoder
        // the display through the GPU (GlScaler): navigation bar left out,
        // box-filtered k:1; without it, Android's one bilinear step blurs text
        var input = e.surface
        var vw = w
        var vh = h
        if (viaGl) {
            val g = h264Geometry(w, h)
            val sc = GlScaler(e.surface!!, w, h, g, fps)
            if (sc.start()) {
                scaler = sc
                input = sc.inputSurface
                vw = g.capW
                vh = g.capH
            } else {
                h264ScalerFailed = sc.error
                AppLog.w("GL scaler failed (${sc.error}): the display draws into the encoder directly")
                // the next start is the direct one, with the encoder's repeats
                e.release()
                return false
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
        mmi.newEncoder(e, caps.h264MaxFrame)
        portrait = false
        AppLog.i("H.264 on: ${e.description}, display ${vw}x$vh${scaler?.let { " -> GL x${it.k}" } ?: " direct"}")
        return true
    }

    /** What the encoder's w x h gets of the screen as it is now. */
    private fun h264Geometry(w: Int, h: Int): ScreenCrop.Geometry {
        readScreen()
        val cw = insets.contentW(screen.w)
        val ch = insets.contentH(screen.h)
        val pic = VideoFit.fit(cw, ch, w, h)
        val k = if (Streaming.sharp) GlBox.factor(cw, ch, w, h) else 1
        return ScreenCrop.geometry(screen.w, screen.h, insets, pic.w, pic.h, k)
    }

    /** A rotation, or the Sharp or navigation switch: the scaler and display
     * follow, with the same encoder, so the MMI doesn't restart. */
    private fun followScreen() {
        val sc = scaler ?: return
        val g = h264Geometry(sc.w, sc.h)
        if (g == sc.geometry) return
        // a failure shows as sc.failed on the next tick: direct from then on
        if (!sc.update(g)) return
        // a new size comes with a new input surface (GlScaler.update): the
        // display draws at the new size only into a new surface
        display?.let { d ->
            d.resize(g.capW, g.capH, resources.displayMetrics.densityDpi)
            d.surface = sc.inputSurface
        }
        sc.dropOld()
        AppLog.i("H.264 capture now $g")
    }

    /** H.264 is landscape only: the notice instead of a portrait screen. */
    private fun followOrientation(e: HwEncoder) {
        readScreen()
        val now = VideoFit.portrait(screen.w, screen.h)
        if (now == portrait) return
        val sc = scaler
        if (sc != null) {
            val card = if (now) Notice.landscapeRequired(sc.w, sc.h) else null
            val ok = sc.showNotice(card)
            card?.recycle()
            // a failure shows as sc.failed on the next tick: direct from then on
            if (!ok) return
        } else {
            display?.surface = if (now) null else e.surface
        }
        portrait = now
        AppLog.i(if (now) "portrait: H.264 shows the landscape notice${if (sc == null) " (direct: the picture stops)" else ""}"
                 else "landscape: H.264 shows the screen again")
    }

    /** The phone can't do H.264: no fallback, streaming stops and
     * [Streaming.status] keeps why. */
    private fun fail(why: String) {
        if (stopReason.isNotEmpty()) return
        AppLog.w("H.264: $why: streaming stops")
        stopReason = "H.264 failed ($why): streaming stopped"
        Streaming.status = stopReason
        stopSelf()
    }

    private fun stopEncoder() {
        val e = enc ?: return
        display?.surface = null
        scaler?.release()                 // nothing draws into the encoder after this
        scaler = null
        e.release()
        mmi.encoderGone()
        AppLog.i("H.264 off")
    }

    /** [screen] and [insets] up to date; the capture thread calls this far
     * more often than a screen rotates, and the insets are a binder call. */
    private fun readScreen() {
        val now = SystemClock.elapsedRealtime()
        if (screen.w != 0 && now - screenAt < SCREEN_CHECK_MS) return
        screenAt = now
        val dm = getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
        val metrics = DisplayMetrics()
        @Suppress("DEPRECATION")
        dm.getDisplay(Display.DEFAULT_DISPLAY).getRealMetrics(metrics)
        screen = VideoFit.Size(metrics.widthPixels, metrics.heightPixels)
        val i = navBarInsets(screen)
        if (i != insets) AppLog.i("screen ${screen.w}x${screen.h}: leaving out $i")
        insets = i
    }

    /** The navigation bar's edge, whether or not the app in front hides the
     * bar: that can't be seen from here. API 30+. */
    private fun navBarInsets(s: VideoFit.Size): ScreenCrop.Insets {
        if (!Streaming.hideNavBar || Build.VERSION.SDK_INT < 30) return ScreenCrop.NONE
        val metrics = getSystemService(WindowManager::class.java).currentWindowMetrics
        // mid-rotation these can still be the old rotation's
        if (metrics.bounds.width() != s.w || metrics.bounds.height() != s.h) return ScreenCrop.NONE
        val bar = metrics.windowInsets.getInsetsIgnoringVisibility(WindowInsets.Type.navigationBars())
        return ScreenCrop.sane(ScreenCrop.Insets(bar.left, bar.top, bar.right, bar.bottom), s.w, s.h)
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
        private const val POLL_MS = 20L   // waiting for the MMI or the encoder
        private const val H264_TICK_MS = 5L
        private const val FPS_SETTLE_MS = 700L
        private const val SCREEN_CHECK_MS = 200L
        // H.264 bit rates of the drop-down (res/values/strings.xml bit_rates);
        // 2.5 Mbit/s is ~12 KB a frame at 25 fps
        // no 8 Mbit/s: its IDRs (up to ~170 KB at 720x432) outgrow the MMI's
        // 128 KB frames
        val BIT_RATES = intArrayOf(2_500_000, 4_000_000, 6_000_000)
        const val DEFAULT_BIT_RATE = 4_000_000

        /** The hardware encoder streaming needs: Android 10+. */
        val phoneCanH264: Boolean get() = Build.VERSION.SDK_INT >= 29

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
    /** Frames a second at most, set by the activity (its slider). */
    @Volatile var maxFps = CaptureService.DEFAULT_FPS
    /** H.264 bits a second, set by the activity (its drop-down). */
    @Volatile var bitRate = CaptureService.DEFAULT_BIT_RATE
    /** H.264 box-filtered k:1 (sharp text) or k = 1 (Android's one scaling
     * step, for comparison), set by the activity. */
    @Volatile var sharp = true
    /** Leave the navigation bar out of the picture, set by the activity. */
    @Volatile var hideNavBar = true
    /** Hold the screen on while streaming, set by the activity (its checkbox). */
    @Volatile var keepScreenOn = false
    /** Send the phone's audio (SBC) to the MMI while mirroring, set by the
     * activity (its checkbox). The MMI only logs it for now. */
    @Volatile var sendAudio = true
}

