package mmi3g.audistreaming

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import android.util.Log
import android.view.Surface

/**
 * The phone's hardware H.264 decoder for Android Auto's video ([mode],
 * Baseline), rendering every frame into [surface] ([GlScaler]'s input) as
 * soon as it is decoded. Callbacks and every call on [handler]'s thread.
 *
 * [config] (the codec config, SPS + PPS in Annex-B) starts it, or restarts
 * it when the config changes; [frame] queues a frame until an input buffer
 * is free. Frames are never dropped (a P-frame needs every one before it);
 * if the decoder falls far behind, the queue is emptied up to the next IDR.
 */
class AaDecoder(
    private val surface: Surface,
    private val mode: AaGeometry.Mode,
    private val handler: Handler,
    private val onError: (String) -> Unit,
) {
    private var codec: MediaCodec? = null
    private var csd: ByteArray? = null
    private val pending = ArrayDeque<Pair<ByteArray, Long>>()
    private val free = ArrayDeque<Int>()
    private var waitIdr = false
    @Volatile var decoded = 0
        private set
    @Volatile var dropped = 0
        private set
    var description = ""
        private set

    /** Codec config from the phone: (re)starts the decoder if it is new. */
    fun config(data: ByteArray) {
        if (codec != null && csd?.contentEquals(data) == true) return
        release()
        val sp = H264.spsPps(data)
        if (sp == null) {
            onError("codec config without SPS/PPS")
            return
        }
        val f = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, mode.w, mode.h).apply {
            setByteBuffer("csd-0", java.nio.ByteBuffer.wrap(START + sp.first))
            setByteBuffer("csd-1", java.nio.ByteBuffer.wrap(START + sp.second))
            setInteger(MediaFormat.KEY_PRIORITY, 0)                // real time
            if (Build.VERSION.SDK_INT >= 30) setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
        }
        val c = try {
            MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        } catch (e: Exception) {
            onError("no H.264 decoder: ${e.message}")
            return
        }
        try {
            if (Build.VERSION.SDK_INT >= 30 &&
                !c.codecInfo.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)
                    .isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)) {
                f.removeKey(MediaFormat.KEY_LOW_LATENCY)
            }
            c.setCallback(object : MediaCodec.Callback() {
                override fun onInputBufferAvailable(codec: MediaCodec, index: Int) {
                    if (this@AaDecoder.codec !== codec) return
                    free.addLast(index)
                    feed()
                }

                override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                    if (this@AaDecoder.codec !== codec) return
                    val show = info.size > 0 && info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0
                    codec.releaseOutputBuffer(index, show)
                    if (show) decoded++
                }

                override fun onError(codec: MediaCodec, e: MediaCodec.CodecException) {
                    if (this@AaDecoder.codec === codec) onError("decoder: ${e.diagnosticInfo}")
                }

                override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {
                    Log.i(TAG, "AA decoder output $format")
                }
            }, handler)
            c.configure(f, surface, null, 0)
            c.start()
        } catch (e: Exception) {
            Log.w(TAG, "AA decoder setup", e)
            try { c.release() } catch (_: Exception) { }
            onError("decoder setup: ${e.message}")
            return
        }
        codec = c
        csd = data
        waitIdr = true
        description = c.name
        AppLog.i("AA decoder: ${c.name}, SPS ${sp.first.size} / PPS ${sp.second.size} B")
    }

    /** One frame (Annex-B) from the phone. */
    fun frame(data: ByteArray, tsUs: Long) {
        if (codec == null) return                        // no config yet
        if (waitIdr) {
            if (!H264.isIdr(data)) {
                dropped++
                return
            }
            waitIdr = false
        }
        if (pending.size >= MAX_PENDING) {
            // far behind: start again at the next IDR
            dropped += pending.size + 1
            pending.clear()
            waitIdr = true
            AppLog.w("AA decoder: $MAX_PENDING frames waiting, dropped up to the next IDR")
            return
        }
        pending.addLast(data to tsUs)
        feed()
    }

    private fun feed() {
        val c = codec ?: return
        while (pending.isNotEmpty() && free.isNotEmpty()) {
            val (data, ts) = pending.removeFirst()
            val index = free.removeFirst()
            try {
                val buf = c.getInputBuffer(index) ?: continue
                buf.clear()
                if (data.size > buf.remaining()) {
                    dropped++
                    c.queueInputBuffer(index, 0, 0, ts, 0)
                    continue
                }
                buf.put(data)
                c.queueInputBuffer(index, 0, data.size, ts, 0)
            } catch (e: IllegalStateException) {
                Log.w(TAG, "AA decoder input", e)
                return
            }
        }
    }

    fun release() {
        val c = codec ?: return
        codec = null
        csd = null
        pending.clear()
        free.clear()
        try { c.stop() } catch (_: Exception) { }
        try { c.release() } catch (_: Exception) { }
    }

    companion object {
        private const val TAG = "AudiStreaming"
        private val START = byteArrayOf(0, 0, 0, 1)
        private const val MAX_PENDING = 60               // 2 s at 30 fps
    }
}
