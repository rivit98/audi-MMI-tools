package mmi3g.usbtx

import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Color
import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/**
 * usb_tx_test sender: connects to the MMI (an Android Open Accessory host
 * running src/usb_tx_test) and sends the typed text.
 *
 * Android starts this activity when the MMI switches the phone into
 * accessory mode (USB_ACCESSORY_ATTACHED, matched by
 * res/xml/accessory_filter.xml), which also grants the permission. When
 * opened by hand it looks for an already attached accessory in onResume.
 *
 * "Stream screen" asks for screen capture and starts CaptureService, which
 * sends the screen to the MMI (~2 pictures a second) through the link
 * published in [Streaming.link]. Back only moves the app to the background,
 * so the link (and the streaming) stays up while another app is used.
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
    private lateinit var input: EditText
    private lateinit var sendButton: Button
    private lateinit var sentList: LinearLayout
    private lateinit var sentScroll: ScrollView
    private lateinit var streamButton: Button
    private lateinit var videoView: TextView
    private val ui = Handler(Looper.getMainLooper())
    private val refresh = object : Runnable {
        override fun run() {
            showVideo()
            ui.postDelayed(this, 500)
        }
    }

    private var link: AccessoryLink? = null
        set(value) {
            field = value
            Streaming.link = value
        }
    private var status = Status.WAITING
    private val sentViews = HashMap<Int, TextView>()

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
        setContentView(R.layout.activity_main)
        usb = getSystemService(Context.USB_SERVICE) as UsbManager
        statusView = findViewById(R.id.status)
        detailView = findViewById(R.id.detail)
        input = findViewById(R.id.input)
        sendButton = findViewById(R.id.send)
        sentList = findViewById(R.id.sent_list)
        sentScroll = findViewById(R.id.sent_scroll)
        streamButton = findViewById(R.id.stream)
        videoView = findViewById(R.id.video_status)
        sendButton.setOnClickListener { send() }
        streamButton.setOnClickListener { toggleStreaming() }

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
        ui.post(refresh)
        if (link == null && status != Status.PERMISSION) {
            usb.accessoryList?.firstOrNull()?.let { acc ->
                if (usb.hasPermission(acc)) openAccessory(acc) else requestPermission(acc)
            }
        }
    }

    override fun onPause() {
        ui.removeCallbacks(refresh)
        super.onPause()
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        moveTaskToBack(true)            // keep the link (and the streaming) up
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQ_CAPTURE) return
        if (resultCode == RESULT_OK && data != null) {
            CaptureService.start(this, resultCode, data)
        } else {
            videoView.text = "Screen capture not allowed"
        }
    }

    private fun toggleStreaming() {
        if (Streaming.running) {
            CaptureService.stop(this)
        } else {
            val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
            @Suppress("DEPRECATION")
            startActivityForResult(mpm.createScreenCaptureIntent(), REQ_CAPTURE)
        }
    }

    private fun showVideo() {
        val l = link
        val caps = l?.videoCaps
        streamButton.text = getString(if (Streaming.running) R.string.stream_stop else R.string.stream_start)
        streamButton.isEnabled = Streaming.running || caps != null
        videoView.text = when {
            Streaming.running && l != null -> {
                val v = l.video
                "Video: ${Streaming.status}; sent ${v.sent}, shown ${v.shown}, " +
                    "bad ${v.bad}, dropped ${v.dropped}; MMI decode ${v.decodeMs} ms, " +
                    "round trip ${v.roundTripMs} ms"
            }
            Streaming.running -> "Video: ${Streaming.status}"
            l == null -> "Video: not connected"
            caps == null -> "Video: this MMI app shows text only"
            else -> "Video: off. MMI shows up to ${caps.landW}x${caps.landH} " +
                "(landscape) / ${caps.portW}x${caps.portH} (portrait)"
        }
    }

    override fun onDestroy() {
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
        setStatus(Status.OPEN, "${l.description}: open usb_tx_test on the MMI (SD card)")
    }

    private fun send() {
        val l = link ?: return
        val text = input.text.toString()
        if (text.isEmpty()) return
        val seq = l.sendText(text)
        if (seq < 0) {
            input.error = "Too long: max ${Proto.MAX_TEXT_BYTES} bytes of UTF-8"
            return
        }
        input.text.clear()
        addSent(seq, text)
    }

    private fun addSent(seq: Int, text: String) {
        val v = TextView(this).apply {
            setPadding(0, 8, 0, 8)
            textSize = 16f
            this.text = "…  #$seq  $text"
        }
        sentViews[seq] = v
        sentList.addView(v)
        sentScroll.post { sentScroll.fullScroll(View.FOCUS_DOWN) }
    }

    private fun setStatus(s: Status, detail: String) {
        status = s
        statusView.text = s.title
        statusView.setBackgroundColor(s.colour)
        statusView.setTextColor(Color.WHITE)
        detailView.text = detail
        sendButton.isEnabled = s == Status.OPEN || s == Status.CONNECTED
    }

    // AccessoryLink.Listener (main thread)

    override fun onPeerHello(peer: String) {
        setStatus(Status.CONNECTED, peer)
    }

    override fun onAck(seq: Int) {
        sentViews.remove(seq)?.let { it.text = "✓" + it.text.toString().removePrefix("…") }
    }

    override fun onClosed(reason: String) {
        link = null
        setStatus(Status.WAITING, "Disconnected: $reason. Plug the phone into the car's USB port")
    }

    companion object {
        private const val ACTION_PERMISSION = "mmi3g.usbtx.USB_PERMISSION"
        private const val REQ_CAPTURE = 1
    }
}
