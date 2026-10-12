package rivit.usbtx

import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
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
 */
class AccessoryLink(
    private val usb: UsbManager,
    private val accessory: UsbAccessory,
    private val listener: Listener,
) {
    interface Listener {
        fun onPeerHello(peer: String)
        fun onAck(seq: Int)
        fun onClosed(reason: String)
    }

    private val main = Handler(Looper.getMainLooper())
    private val writer: ExecutorService = Executors.newSingleThreadExecutor()
    private var pfd: ParcelFileDescriptor? = null
    private var output: FileOutputStream? = null
    @Volatile private var closed = false
    private var nextSeq = 1

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

    /** Queues a TEXT frame; returns its seq, or -1 if the text is too long. */
    fun sendText(text: String): Int {
        val seq = nextSeq
        val frame = Proto.textOrNull(seq, text) ?: return -1
        nextSeq = if (nextSeq == 0xffff) 1 else nextSeq + 1
        write(frame)
        return seq
    }

    fun close(reason: String) {
        if (closed) return
        closed = true
        Log.i(TAG, "close: $reason")
        writer.shutdownNow()
        // Closing the fd makes a blocked read/write fail; the reader may
        // still sit in read() until the host sends something - it is a
        // daemon thread and ignores everything once `closed` is set.
        try { pfd?.close() } catch (_: IOException) { }
        main.post { listener.onClosed(reason) }
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
            Proto.ACK -> if (payload.size >= 2) {
                val seq = Proto.u16(payload, 0)
                main.post { if (!closed) listener.onAck(seq) }
            }
            else -> Log.w(TAG, "unknown frame type $type, ${payload.size} bytes")
        }
    }

    companion object {
        private const val TAG = "usb_tx_test"
        val HELLO = "usb_tx_test-android/1 ${Build.MANUFACTURER} ${Build.MODEL}"
    }
}
