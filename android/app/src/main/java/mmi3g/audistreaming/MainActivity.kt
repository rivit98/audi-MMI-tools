package mmi3g.audistreaming

import android.app.Activity
import android.app.AlertDialog
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Color
import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.AdapterView
import android.widget.Button
import android.widget.Spinner
import android.widget.TextView

/**
 * Audi streaming: connects to the MMI (an Android Open Accessory host
 * running src/audi_stream_receiver) and makes this app the head unit for
 * Android Auto on this phone ([AaService]): the MMI shows Android Auto, and
 * the page shows the same picture to touch. (There are no typed text
 * messages: src/audi_stream_receiver/README.md.)
 *
 * Android starts this activity when the MMI switches the phone into
 * accessory mode (USB_ACCESSORY_ATTACHED, matched by
 * res/xml/accessory_filter.xml), which also grants the permission. When
 * opened by hand it looks for an already attached accessory in onResume.
 *
 * The Android Auto mode starts with the activity; Start / Stop switch it.
 * Its H.264 settings ("Max fps", "H.264 rate", "Keep screen on",
 * [StreamOptions]) and the video size are remembered. Every option has an
 * info button that explains it (setUpInfo). Back only moves the app to the
 * background, so the link (and the streaming) stays up while another app is
 * used. The MMI link is the activity's, published in [Streaming.link].
 *
 * Turning the phone must not recreate the activity (configChanges in the
 * manifest): otherwise every rotation closes the link and opened a new
 * one, and the old activity's late onClosed then cleared Streaming.link,
 * so the service waited for an MMI that was connected all along. The link
 * setter, too, only ever clears the link it set itself.
 */
class MainActivity : Activity(), AccessoryLink.Listener {
    private enum class Status(val colour: Int, val title: String) {
        WAITING(0xFF5A6472.toInt(), "Waiting for MMI"),
        PERMISSION(0xFFB07A10.toInt(), "Waiting for USB permission"),
        OPEN(0xFF2F6FA8.toInt(), "USB open, waiting for MMI app"),
        CONNECTED(0xFF2E8E4A.toInt(), "Connected to MMI"),
    }

    private lateinit var usb: UsbManager
    private lateinit var statusView: TextView
    private lateinit var detailView: TextView
    private lateinit var aaStatus: TextView
    private lateinit var aaToggle: Button
    private val ui = Handler(Looper.getMainLooper())
    private val refresh = object : Runnable {
        override fun run() {
            showVideo()
            ui.postDelayed(this, 500)
        }
    }

    private var link: AccessoryLink? = null
        set(value) {
            if (value != null || Streaming.link === field) Streaming.link = value
            field = value
        }
    private var status = Status.WAITING

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val acc = accessoryOf(intent)
            when (intent.action) {
                UsbManager.ACTION_USB_ACCESSORY_DETACHED ->
                    link?.close("USB cable disconnected")
                ACTION_PERMISSION ->
                    if (acc != null && intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                        openAccessory(acc)
                    } else {
                        setStatus(Status.WAITING, "USB permission denied - replug the cable")
                    }
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        AppLog.init(this)
        AppLog.i("activity created")
        setContentView(R.layout.activity_main)
        findViewById<TextView>(R.id.version).text =
            getString(R.string.version, packageManager.getPackageInfo(packageName, 0).versionName)
        usb = getSystemService(Context.USB_SERVICE) as UsbManager
        statusView = findViewById(R.id.status)
        detailView = findViewById(R.id.detail)
        val prefs = getPreferences(MODE_PRIVATE)
        StreamOptions.load(prefs)
        StreamOptions(findViewById(R.id.aa_options), prefs) { fps -> getString(R.string.max_fps, fps) }
        setUpInfo(findViewById(R.id.aa_options), OPTION_INFO)
        setUpInfo()
        setUpAa()

        val filter = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_ACCESSORY_DETACHED)
            addAction(ACTION_PERMISSION)
        }
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(receiver, filter)
        }
        setStatus(Status.WAITING, "Plug the phone into the car's USB port")
        handleIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    override fun onResume() {
        super.onResume()
        AppLog.i("activity resumed")
        ui.post(refresh)
        if (link == null && status != Status.PERMISSION) {
            usb.accessoryList?.firstOrNull()?.let { acc ->
                if (usb.hasPermission(acc)) openAccessory(acc) else requestPermission(acc)
            }
        }
    }

    override fun onPause() {
        AppLog.i("activity paused")
        ui.removeCallbacks(refresh)
        super.onPause()
    }

    override fun onStop() {
        AppLog.i("activity stopped")
        super.onStop()
    }

    // lost when the notification shade or a dialog opens over the activity
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        AppLog.i("activity window focus: $hasFocus")
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        moveTaskToBack(true)            // keep the link (and the streaming) up
    }


    /** The Android Auto page's controls; the mode starts. */
    private fun setUpAa() {
        aaStatus = findViewById(R.id.aa_status)
        aaToggle = findViewById(R.id.aa_toggle)
        aaToggle.setOnClickListener {
            if (AaService.running) AaService.stop(this) else AaService.start(this)
        }
        findViewById<Button>(R.id.aa_settings).setOnClickListener { openAaSettings() }
        setUpVideoSize()
        AaService.start(this)
    }

    /** Android Auto's video size, remembered; a new one reconnects. */
    private fun setUpVideoSize() {
        val prefs = getPreferences(MODE_PRIVATE)
        val modes = AaGeometry.MODES
        val saved = modes.indexOfFirst { it.toString() == prefs.getString(PREF_AA_VIDEO_SIZE, null) }.coerceAtLeast(0)
        AaService.setMode(modes[saved])
        val preview = findViewById<View>(R.id.aa_preview)
        findViewById<Spinner>(R.id.aa_video_size).apply {
            setSelection(saved)
            onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(parent: AdapterView<*>, view: View?, pos: Int, id: Long) {
                    val m = modes[pos]
                    if (m == AaService.mode) return
                    AppLog.i("AA mode: video size $m")
                    prefs.edit().putString(PREF_AA_VIDEO_SIZE, m.toString()).apply()
                    AaService.setMode(m)
                    preview.requestLayout()
                }
                override fun onNothingSelected(parent: AdapterView<*>) {}
            }
        }
    }

    /** Android Auto's own settings (its developer menu has the head unit
     * server), or its app info if that screen can't be opened. */
    private fun openAaSettings() {
        val settings = Intent().setClassName(AA_PACKAGE,
            "com.google.android.projection.gearhead.companion.settings.DefaultSettingsActivity")
        try {
            startActivity(settings)
        } catch (e: Exception) {
            AppLog.w("Android Auto settings: ${e.message}")
            try {
                startActivity(Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS)
                    .setData(android.net.Uri.parse("package:$AA_PACKAGE")))
            } catch (e2: Exception) {
                aaStatus.text = "Android Auto is not installed"
            }
        }
    }

    /** The (i) buttons stay usable while their option is disabled. */
    private fun setUpInfo(root: View = window.decorView, info: List<Triple<Int, Int, Int>> = INFO) {
        for ((button, title, text) in info) {
            root.findViewById<View>(button).apply {
                contentDescription = getString(R.string.info_about, getString(title))
                setOnClickListener {
                    AlertDialog.Builder(this@MainActivity)
                        .setTitle(title)
                        .setMessage(text)
                        .setPositiveButton(android.R.string.ok, null)
                        .show()
                }
            }
        }
    }

    private fun showVideo() {
        aaToggle.text = getString(if (AaService.running) R.string.aa_stop else R.string.aa_start)
        aaStatus.text = if (AaService.running) AaService.status else getString(R.string.aa_off)
    }

    override fun onDestroy() {
        AppLog.i("activity destroyed")
        unregisterReceiver(receiver)
        link?.close("app closed")
        link = null
        super.onDestroy()
    }

    private fun handleIntent(intent: Intent?) {
        if (intent?.action == UsbManager.ACTION_USB_ACCESSORY_ATTACHED) {
            accessoryOf(intent)?.let { openAccessory(it) }
        }
    }

    @Suppress("DEPRECATION")
    private fun accessoryOf(intent: Intent): UsbAccessory? =
        if (Build.VERSION.SDK_INT >= 33) {
            intent.getParcelableExtra(UsbManager.EXTRA_ACCESSORY, UsbAccessory::class.java)
        } else {
            intent.getParcelableExtra(UsbManager.EXTRA_ACCESSORY)
        }

    private fun requestPermission(acc: UsbAccessory) {
        setStatus(Status.PERMISSION, "${acc.manufacturer} ${acc.model}")
        val intent = Intent(ACTION_PERMISSION).setPackage(packageName)
        // MUTABLE: the system adds the accessory + granted extras to it.
        val flags = if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0
        usb.requestPermission(acc, PendingIntent.getBroadcast(this, 0, intent, flags))
    }

    private fun openAccessory(acc: UsbAccessory) {
        if (link != null) return
        val l = AccessoryLink(usb, acc, this)
        if (!l.open()) {
            setStatus(Status.WAITING, "Could not open ${acc.manufacturer} ${acc.model} - replug the cable")
            return
        }
        link = l
        setStatus(Status.OPEN, "${l.description}: open audi_stream_receiver on the MMI (SD card)")
    }

    private fun setStatus(s: Status, detail: String) {
        status = s
        statusView.text = s.title
        statusView.setBackgroundColor(s.colour)
        statusView.setTextColor(Color.WHITE)
        detailView.text = detail
    }

    // AccessoryLink.Listener (main thread)

    override fun onPeerHello(peer: String) {
        setStatus(Status.CONNECTED, peer)
    }

    override fun onClosed(closed: AccessoryLink, reason: String) {
        if (closed !== link) return     // an older link, already replaced
        link = null
        setStatus(Status.WAITING, "Disconnected: $reason. Plug the phone into the car's USB port")
    }

    companion object {
        private const val ACTION_PERMISSION = "mmi3g.audistreaming.USB_PERMISSION"
        private const val PREF_AA_VIDEO_SIZE = "aa_video_size"
        private const val AA_PACKAGE = "com.google.android.projection.gearhead"
        private val INFO = listOf(
            Triple(R.id.info_aa, R.string.info_aa_title, R.string.info_aa),
            Triple(R.id.info_aa_video_size, R.string.info_aa_video_size_title, R.string.info_aa_video_size),
        )
        // in res/layout/stream_options.xml
        private val OPTION_INFO = listOf(
            Triple(R.id.info_fps, R.string.info_fps_title, R.string.info_fps),
            Triple(R.id.info_bit_rate, R.string.info_bit_rate_title, R.string.info_bit_rate),
            Triple(R.id.info_keep_screen_on, R.string.info_keep_screen_on_title, R.string.info_keep_screen_on),
        )
    }
}
