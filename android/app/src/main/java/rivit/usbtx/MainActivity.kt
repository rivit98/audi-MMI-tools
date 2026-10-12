package rivit.usbtx

import android.app.Activity
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

    private var link: AccessoryLink? = null
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
        sendButton.setOnClickListener { send() }

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
        if (link == null && status != Status.PERMISSION) {
            usb.accessoryList?.firstOrNull()?.let { acc ->
                if (usb.hasPermission(acc)) openAccessory(acc) else requestPermission(acc)
            }
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
        private const val ACTION_PERMISSION = "rivit.usbtx.USB_PERMISSION"
    }
}
