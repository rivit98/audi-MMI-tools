package mmi3g.audistreaming

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.view.MotionEvent
import android.view.Surface
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * Android Auto mode: this app is the Android Auto head unit for Android
 * Auto on the same phone ("self mode"), and the MMI shows its picture as
 * H.264, decoded by its DM642 (docs/android-auto.md, "Phone relay").
 *
 * ```
 * Android Auto (head unit server, 127.0.0.1:5277) --TCP--> AaNative session
 *   video 800x480 H.264 --> AaDecoder --> GlScaler (whole frame -> 720x432, bilinear, 25 fps;
 *                                         also the touch preview) --> HwEncoder 720x432
 *   --> MmiH264 --> AccessoryLink --> MMI (audi_stream_receiver, DM642)
 *   preview touches --> AaNative.touch --> Android Auto
 * ```
 *
 * Android Auto 17.4+ only starts a projection itself through its developer
 * "Head unit server" (open-headunit's finding): Android Auto settings ->
 * tap the version until developer mode is on -> menu -> Start head unit
 * server. This service connects to it on 127.0.0.1:5277, and tries again
 * every [RETRY_MS] until it answers and after every session.
 *
 * The session asks for the chosen video size ([mode], [AaGeometry]): the
 * whole frame scaled into the MMI's 720x432 H.264 picture with its shape
 * kept, or ("800x480 1:1") margins + ui_config and the UI's 720x464 copied
 * 1:1. Only system sounds are advertised: music and navigation stay
 * on the phone (Bluetooth to the car); system sounds play here ([AaAudio]).
 *
 * The H.264 settings are [Streaming]'s ([StreamOptions]): "Max fps"
 * is the frame clock's rate (what Android Auto sends above it is dropped),
 * then "H.264 rate" and "Keep screen on". A new rate or bit
 * rate, once settled, gets a new encoder that the scaler draws into
 * ([GlScaler.retarget]); the decoder goes on, so Android Auto never
 * notices, and only the MMI restarts its stream.
 *
 * Everything runs on one thread ("aa"): the session (single-threaded), the
 * decoder's and encoder's callbacks, [MmiH264]. The socket has a reader and
 * a writer thread of its own. STREAM_SETTINGS tells the MMI's log what
 * the stream runs with.
 */
class AaService : Service() {
    private lateinit var thread: HandlerThread
    private lateinit var handler: Handler
    private val main = Handler(Looper.getMainLooper())
    @Volatile private var stopping = false

    // the socket to Android Auto (net thread connects, reader reads)
    private var socket: Socket? = null
    private var output: OutputStream? = null
    private var writer: ExecutorService? = null
    private var netThread: Thread? = null
    @Volatile private var generation = 0            // a new socket: older readers' posts are ignored

    // aa thread
    private var session: AaNative? = null
    private var cert = ""
    private var key = ""
    private val mmi = MmiH264()
    private var scaler: GlScaler? = null
    private var decoder: AaDecoder? = null
    private val audio = AaAudio()
    private var previewSurface: Surface? = null
    private var pipelineFailed = ""
    // the video size of the current session ([mode] when it started)
    @Volatile private var sessionMode = AaGeometry.MODES[0]
    private var sessions = 0
    private var lastEnd = ""
    private var statusAt = 0L
    private var fpsAt = 0L
    private var fpsDecoded = 0
    private var fps = 0.0
    private var settingsChangeAt = 0L
    private var settingsLink: AccessoryLink? = null   // the link the settings went to
    private var settingsSent = ""                     // and what they were
    private lateinit var screenLock: PowerManager.WakeLock

    private val tick = Runnable { onTick() }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        AppLog.init(this)
        thread = HandlerThread("aa").apply { start() }
        handler = Handler(thread.looper)
        val power = getSystemService(Context.POWER_SERVICE) as PowerManager
        @Suppress("DEPRECATION")
        screenLock = power.newWakeLock(PowerManager.SCREEN_DIM_WAKE_LOCK, "audistreaming:aa_keep_screen_on")
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            AppLog.i("AA mode: stop asked")
            stopSelf()
            return START_NOT_STICKY
        }
        // every start: startForegroundService wants it each time
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTIFICATION_ID, notification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } else {
            startForeground(NOTIFICATION_ID, notification())
        }
        if (instance === this) return START_NOT_STICKY
        try {
            cert = assets.open("aa/headunit.crt").bufferedReader().readText()
            key = assets.open("aa/headunit.key").bufferedReader().readText()
        } catch (e: IOException) {
            AppLog.w("AA mode: no head unit certificate in the app", e)
            status = "No head unit certificate in the app (build problem)"
            stopSelf()
            return START_NOT_STICKY
        }
        instance = this
        handler.post { previewSurface = previewWanted }
        running = true
        status = "Starting"
        AppLog.i("AA mode on")
        startNet()
        handler.post(tick)
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        stopping = true
        AppLog.i("AA mode off")
        running = false
        connected = false
        status = ""
        closeSocket("AA mode off")
        netThread?.interrupt()
        handler.removeCallbacksAndMessages(null)
        handler.post {
            session?.let {
                it.bye()
                it.close()
            }
            session = null
            drainLog()
            stopPipeline()
            audio.release()
            if (screenLock.isHeld) screenLock.release()
            Streaming.link?.sendVideoStop()
            // the preview's owner may let its surface go from here on
            if (instance === this) instance = null
            thread.quitSafely()
        }
        super.onDestroy()
    }

    // --- the socket (net thread) -------------------------------------------

    /** Connects to Android Auto's head unit server until stopped: a session
     * at a time, a new attempt [RETRY_MS] after each. */
    private fun startNet() {
        netThread = Thread({
            var announced = false
            while (!stopping) {
                val s = Socket()
                try {
                    s.tcpNoDelay = true
                    s.connect(InetSocketAddress("127.0.0.1", PORT), CONNECT_TIMEOUT_MS)
                } catch (e: IOException) {
                    try { s.close() } catch (_: IOException) { }
                    if (!announced) {
                        AppLog.i("AA mode: no head unit server on 127.0.0.1:$PORT (${e.message})")
                        announced = true
                    }
                    if (!connected) status = NO_SERVER
                    if (!pause(RETRY_MS)) return@Thread
                    continue
                }
                announced = false
                val input = try { s.getInputStream() } catch (e: IOException) { null }
                if (input == null) {
                    try { s.close() } catch (_: IOException) { }
                    continue
                }
                val gen = opened(s)
                read(input, gen)               // until the socket closes
                closeSocket("Android Auto closed the connection")
                if (!pause(RETRY_MS)) return@Thread
            }
        }, "aa-net").apply { isDaemon = true; start() }
    }

    private fun pause(ms: Long): Boolean = try {
        Thread.sleep(ms)
        !stopping
    } catch (_: InterruptedException) {
        false
    }

    @Synchronized
    private fun opened(s: Socket): Int {
        socket = s
        output = s.getOutputStream()
        writer = Executors.newSingleThreadExecutor()
        val gen = ++generation
        connected = true
        AppLog.i("AA mode: connected to the head unit server")
        handler.post { startSession(gen) }
        return gen
    }

    private fun read(input: InputStream, gen: Int) {
        val buf = ByteArray(65536)
        try {
            while (!stopping) {
                val n = input.read(buf)
                if (n < 0) break
                val copy = buf.copyOf(n)
                handler.post { if (gen == generation) session?.rx(copy, copy.size) }
            }
        } catch (e: IOException) {
            if (!stopping) AppLog.i("AA mode: read ended: ${e.message}")
        }
        handler.post { if (gen == generation) endSession("connection closed") }
    }

    @Synchronized
    private fun closeSocket(why: String) {
        val s = socket ?: return
        socket = null
        output = null
        writer?.shutdown()
        writer = null
        connected = false
        AppLog.i("AA mode: socket closed: $why")
        try { s.close() } catch (_: IOException) { }
    }

    /** Ends the session politely (bye-bye), then closes the socket in case
     * Android Auto doesn't; the net thread connects again. */
    private fun reconnect(why: String) {
        handler.post {
            AppLog.i("AA mode: reconnecting: $why")
            session?.bye()
            handler.postDelayed({ closeSocket(why) }, BYE_WAIT_MS)
        }
    }

    @Synchronized
    private fun write(data: ByteArray) {
        val out = output ?: return
        try {
            writer?.execute {
                try {
                    out.write(data)
                } catch (e: IOException) {
                    AppLog.i("AA mode: write failed: ${e.message}")
                    closeSocket("write failed")
                }
            }
        } catch (_: java.util.concurrent.RejectedExecutionException) {
            // the socket is closing: nothing more goes out
        }
    }

    // --- the session (aa thread) ---------------------------------------------

    // Each one catches its own: an exception must not travel back into
    // the native session.
    private val callbacks = object : AaNative.Callbacks {
        override fun send(data: ByteArray) = guard("send") { write(data) }

        override fun phase(phase: Int, detail: String) = guard("phase") {
            AppLog.i("AA mode: ${AaNative.PHASE_NAMES.getOrElse(phase) { "?" }}: $detail")
            if (phase == AaNative.PHASE_ENDED || phase == AaNative.PHASE_FAILED) {
                lastEnd = (if (phase == AaNative.PHASE_FAILED) "failed: " else "") + detail
                closeSocket(lastEnd)
            }
        }

        override fun video(data: ByteArray, tsUs: Long, config: Boolean) = guard("video") {
            if (config) {
                if (startPipeline()) decoder?.config(data)
            } else {
                decoder?.frame(data, tsUs)
            }
        }

        override fun audio(channel: Int, data: ByteArray) = guard("audio") { audio.play(channel, data) }
    }

    private inline fun guard(what: String, body: () -> Unit) {
        try {
            body()
        } catch (e: RuntimeException) {
            AppLog.w("AA mode: $what", e)
        }
    }

    private fun startSession(gen: Int) {
        if (gen != generation || stopping) return
        session?.close()
        val m = mode
        sessionMode = m
        AppLog.i("AA mode: asking for ${m} video at ${m.dpi} dpi")
        val s = AaNative(callbacks, true, false, m.marginW, m.marginH, m.oneToOne, m.w, m.h, m.dpi, "Audi MMI")
        session = s
        sessions++
        fpsDecoded = 0
        if (!s.start(cert, key)) AppLog.w("AA mode: session did not start: ${s.error}")
    }

    private fun endSession(why: String) {
        val s = session ?: return
        val st = s.stats()
        AppLog.i("AA mode: session over ($why): ${st[0]} video frames, ${st[1] / 1024} KB, " +
            "${st[8]} touches, rtt ${st[6]} ms")
        if (lastEnd.isEmpty()) lastEnd = why
        s.close()
        session = null
        drainLog()
        stopPipeline()
    }

    private fun onTick() {
        if (stopping) return
        val s = session
        var next = TICK_MS
        if (s != null) next = s.tick(TICK_MS.toInt()).toLong().coerceIn(1L, TICK_MS)
        drainLog()
        followSettings()
        sendSettings()
        sendToMmi()
        showStatus()
        handler.postDelayed(tick, next)
    }

    private fun drainLog() {
        for (line in AaNative.drainLog()) AppLog.i("aa-native $line")
    }

    // --- decoder -> scaler -> encoder -> MMI (aa thread) -------------------

    /** The H.264 rate the settings ask for: "Max fps", at most the MMI's. */
    private fun wantFps(): Int {
        val mmiFps = Streaming.link?.videoCaps?.h264Fps?.takeIf { it > 0 } ?: Streaming.MAX_FPS
        return Streaming.maxFps.coerceIn(Streaming.MIN_FPS, mmiFps.coerceAtMost(Streaming.MAX_FPS))
    }

    private fun newEncoder(fps: Int): HwEncoder? {
        val e = HwEncoder(sessionMode.picW, sessionMode.picH, fps, Streaming.bitRate, handler, mmi::onConfig, mmi::onFrame) { why ->
            AppLog.w("AA mode H.264: $why")
            pipelineFailed = why
        }
        return if (e.start()) e else null
    }

    /** "Keep screen on", and a new rate or bit rate: once settled for
     * [SETTLE_MS] (a slider being dragged), a new encoder. */
    private fun followSettings() {
        if (Streaming.keepScreenOn != screenLock.isHeld) {
            if (Streaming.keepScreenOn) screenLock.acquire() else screenLock.release()
            AppLog.i("AA mode: keep screen on: ${Streaming.keepScreenOn}")
        }
        val e = mmi.enc
        val sc = scaler
        if (e == null || sc == null) {
            settingsChangeAt = 0L
            return
        }
        val fps = wantFps()
        if (e.fps == fps && e.bitRate == Streaming.bitRate) {
            settingsChangeAt = 0L
            return
        }
        val now = SystemClock.elapsedRealtime()
        if (settingsChangeAt == 0L) settingsChangeAt = now
        if (now - settingsChangeAt < SETTLE_MS) return
        settingsChangeAt = 0L
        val n = newEncoder(fps) ?: return
        if (!sc.retarget(n.surface!!, fps)) {
            AppLog.w("AA mode: GL scaler could not move to the new encoder: ${sc.error}")
            pipelineFailed = "GL: ${sc.error}"
            n.release()
            stopPipeline()
            return
        }
        e.release()
        mmi.newEncoder(n, Streaming.link?.videoCaps?.h264MaxFrame ?: MmiH264.START_FRAME_BYTES * 4)
        AppLog.i("AA mode: new encoder for the settings: ${n.description}")
    }

    /** The picture path, made on the first codec config of a session. */
    private fun startPipeline(): Boolean {
        if (decoder != null) return true
        val caps = Streaming.link?.videoCaps
        val fps = wantFps()
        val e = newEncoder(fps) ?: return false
        val g = AaGeometry.geometry(sessionMode)
        val sc = GlScaler(e.surface!!, g.picW, g.picH, g, fps)
        sc.previewWhole = true
        if (!sc.start()) {
            pipelineFailed = "GL: ${sc.error}"
            AppLog.w("AA mode: GL scaler failed: ${sc.error}")
            e.release()
            return false
        }
        val input = sc.inputSurface
        if (input == null) {
            sc.release()
            e.release()
            return false
        }
        previewSurface?.let { sc.setPreview(it) }
        scaler = sc
        mmi.newEncoder(e, caps?.h264MaxFrame ?: MmiH264.START_FRAME_BYTES * 4)
        decoder = AaDecoder(input, sessionMode, handler) { why ->
            AppLog.w("AA mode: $why")
            pipelineFailed = why
        }
        pipelineFailed = ""
        AppLog.i("AA mode: picture path up: ${e.description}, Android Auto's $sessionMode, rail ${sessionMode.rail} px left out, " +
            "${g.keptW}x${g.keptH} -> ${g.picW}x${g.picH}${if (g.scaled) "" else " (1:1)"}")
        return true
    }

    private fun stopPipeline() {
        decoder?.release()
        decoder = null
        scaler?.release()                     // nothing draws into the encoder after this
        scaler = null
        mmi.enc?.release()
        mmi.encoderGone()
    }

    /** STREAM_SETTINGS to the link when a stream starts on it or a setting
     * changes: the MMI logs them; up= (ms since the phone booted) lines its
     * log up with ours. */
    private fun sendSettings() {
        val link = Streaming.link ?: return
        val e = mmi.enc ?: return
        if (link.videoCaps == null) return                    // no MMI app yet
        val text = "mode=aa video=${sessionMode.w}x${sessionMode.h}${if (sessionMode.oneToOne) "_1:1" else ""} " +
            "pic=${e.w}x${e.h} max_fps=${Streaming.maxFps} bit_rate_kbit=${Streaming.bitRate / 1000} " +
            "keep_screen_on=${if (Streaming.keepScreenOn) 1 else 0} " +
            "frame_kb=${mmi.frameBytes / 1024} android=${Build.VERSION.SDK_INT}"
        if (link === settingsLink && text == settingsSent) return
        settingsLink = link
        settingsSent = text
        val line = "up=${SystemClock.elapsedRealtime()} $text"
        link.sendSettings(line)
        AppLog.i("AA mode: settings sent: $line")
    }

    private fun sendToMmi() {
        val link = Streaming.link ?: return
        val caps = link.videoCaps ?: return
        if (!caps.h264 || mmi.enc == null) return
        if (mmi.refused(link)) return         // tried again after a wait (MmiH264)
        mmi.step(link)
    }

    private fun showStatus() {
        val now = SystemClock.elapsedRealtime()
        val d = decoder
        if (now - fpsAt >= 2000) {
            val n = d?.decoded ?: 0
            fps = if (fpsAt == 0L || n < fpsDecoded) 0.0 else (n - fpsDecoded) * 1000.0 / (now - fpsAt)
            fpsDecoded = n
            fpsAt = now
        }
        val s = session
        val link = Streaming.link
        val e = mmi.enc
        status = when {
            s == null && !connected -> NO_SERVER + if (lastEnd.isNotEmpty()) " Last session: $lastEnd." else ""
            s == null -> "Connected, starting"
            else -> {
                val phase = AaNative.PHASE_NAMES.getOrElse(s.phase) { "?" }
                val phone = s.phone.ifEmpty { "Android Auto" }
                "$phone: $phase" +
                    (if (d != null) "; video %.1f fps decoded".format(fps) +
                        (if (d.dropped > 0) ", ${d.dropped} dropped" else "") else "; no video yet") +
                    when {
                        pipelineFailed.isNotEmpty() -> "; picture path failed: $pipelineFailed"
                        link == null -> "; MMI not connected"
                        link.videoCaps?.h264 != true -> "; the MMI app takes no H.264"
                        link === mmi.refusedLink -> "; the MMI refused H.264, trying again soon"
                        e != null -> "; MMI: " + mmi.describe(e) + if (link.h264.playing) "" else ", MMI starting"
                        else -> ""
                    }
            }
        }
        if (now - statusAt >= STATUS_LOG_MS) {
            statusAt = now
            AppLog.i("AA status: $status; preview ${scaler?.previewDrawn ?: 0} drawn, " +
                "audio dropped ${audio.droppedBytes} B")
        }
    }

    // --- the activity's preview and touches (main thread -> aa thread) ----

    private fun preview(s: Surface?, done: Runnable?) {
        val posted = handler.post {
            previewSurface = s
            scaler?.setPreview(s)
            done?.let { main.post(it) }
        }
        if (!posted) done?.run()               // the thread has ended: nothing draws
    }

    private fun touch(e: MotionEvent, viewW: Int, viewH: Int) {
        val action = when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> AaNative.TOUCH_DOWN
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> AaNative.TOUCH_UP
            MotionEvent.ACTION_MOVE -> AaNative.TOUCH_MOVED
            MotionEvent.ACTION_POINTER_DOWN -> AaNative.TOUCH_POINTER_DOWN
            MotionEvent.ACTION_POINTER_UP -> AaNative.TOUCH_POINTER_UP
            else -> return
        }
        val n = e.pointerCount
        val xs = IntArray(n)
        val ys = IntArray(n)
        val ids = IntArray(n)
        for (i in 0 until n) {
            val (x, y) = AaGeometry.touch(e.getX(i), e.getY(i), viewW, viewH, sessionMode)
            xs[i] = x
            ys[i] = y
            ids[i] = e.getPointerId(i)
        }
        val index = e.actionIndex
        handler.post { session?.touch(action, index, xs, ys, ids) }
    }

    private fun notification(): Notification {
        val stop = PendingIntent.getService(this, 0,
            Intent(this, AaService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE)
        val open = PendingIntent.getActivity(this, 0,
            Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val b = if (Build.VERSION.SDK_INT >= 26) {
            val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            nm.createNotificationChannel(NotificationChannel(CHANNEL, "Android Auto mode",
                NotificationManager.IMPORTANCE_LOW))
            Notification.Builder(this, CHANNEL)
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(this)
        }
        @Suppress("DEPRECATION")
        return b.setSmallIcon(R.drawable.ic_stat_stream)
            .setContentTitle("Android Auto on the MMI")
            .setContentIntent(open)
            .addAction(Notification.Action.Builder(null, "Stop", stop).build())
            .setOngoing(true)
            .build()
    }

    companion object {
        private const val CHANNEL = "aa"
        private const val NOTIFICATION_ID = 2
        private const val ACTION_STOP = "mmi3g.audistreaming.STOP_AA"
        const val PORT = 5277
        private const val CONNECT_TIMEOUT_MS = 1000
        private const val RETRY_MS = 2000L
        private const val TICK_MS = 20L
        private const val BYE_WAIT_MS = 300L
        private const val STATUS_LOG_MS = 5000L
        private const val SETTLE_MS = 700L
        const val NO_SERVER = "Waiting for Android Auto's head unit server on this phone."

        @Volatile private var instance: AaService? = null
        @Volatile private var previewWanted: Surface? = null
        @Volatile var running = false
            private set
        @Volatile var connected = false
            private set
        @Volatile var status = ""
            private set

        /** The video size asked for (MainActivity's setting, remembered). */
        @Volatile var mode = AaGeometry.MODES[0]
            private set

        /** A new video size: a running session ends and the next one (a
         * reconnect, [RETRY_MS] later) asks for it. */
        fun setMode(m: AaGeometry.Mode) {
            if (m == mode) return
            mode = m
            instance?.reconnect("video size $m")
        }

        fun start(context: Context) {
            val i = Intent(context, AaService::class.java)
            if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(i) else context.startService(i)
        }

        fun stop(context: Context) {
            if (!running) return
            context.startService(Intent(context, AaService::class.java).setAction(ACTION_STOP))
        }

        /** The activity's preview surface (null: gone); [done] runs on the
         * main thread once the GL thread has let go of the old one. */
        fun setPreview(s: Surface?, done: Runnable?) {
            previewWanted = s
            val i = instance
            if (i == null) done?.run() else i.preview(s, done)
        }

        /** A touch on a viewW x viewH preview of the UI. */
        fun touch(e: MotionEvent, viewW: Int, viewH: Int) {
            instance?.touch(e, viewW, viewH)
        }
    }
}
