package mmi3g.audistreaming

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.util.Log
import android.view.Surface

/**
 * The phone's hardware H.264 encoder (MediaCodec) for the MMI's DM642:
 * Baseline, level 3.0, w x h, fed from a Surface (the screen capture's
 * virtual display draws into [surface]).
 *
 * - **Rate:** at most fps frames a second reach the encoder
 *   (KEY_MAX_FPS_TO_ENCODER, Android 10+), and when the screen stands
 *   still the last frame is repeated after 1/fps (KEY_REPEAT_PREVIOUS_FRAME_
 *   AFTER), so frames keep coming at about fps either way. The MMI's
 *   stream runs at 25 fps and fills what is missing with skip frames.
 * - **Frame size:** constant bit rate where the encoder offers it, and an
 *   IDR only at the start and when asked ([requestSync]): every frame must
 *   fit the MMI's per-frame size, and IDRs are the big ones.
 *
 * Callbacks run on [handler]'s thread: onConfig(sps, pps) once (codec
 * config), onFrame(Annex-B bytes, isKey) per frame, onError(text).
 */
class HwEncoder(
    val w: Int,
    val h: Int,
    val fps: Int,
    val bitRate: Int,
    private val handler: Handler,
    private val onConfig: (ByteArray, ByteArray) -> Unit,
    private val onFrame: (ByteArray, Boolean) -> Unit,
    private val onError: (String) -> Unit,
) {
    private var codec: MediaCodec? = null
    var surface: Surface? = null
        private set
    var description = ""
        private set

    /** Configure and start; false (and onError) if the phone can't. */
    fun start(): Boolean {
        val c = try {
            MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        } catch (e: Exception) {
            onError("no H.264 encoder: ${e.message}")
            return false
        }
        val caps = c.codecInfo.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)
        val f = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, w, h).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitRate)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, I_FRAME_INTERVAL_S)
            setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)
            setInteger(MediaFormat.KEY_LEVEL, MediaCodecInfo.CodecProfileLevel.AVCLevel3)
            setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 1_000_000L / fps)
            setInteger(MediaFormat.KEY_PRIORITY, 0)           // real time
            if (Build.VERSION.SDK_INT >= 29) setFloat(MediaFormat.KEY_MAX_FPS_TO_ENCODER, fps.toFloat())
            if (caps.encoderCapabilities.isBitrateModeSupported(
                    MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)) {
                setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
            }
            if (Build.VERSION.SDK_INT >= 30 &&
                caps.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)) {
                setInteger(MediaFormat.KEY_LATENCY, 1)
            }
        }
        try {
            c.setCallback(object : MediaCodec.Callback() {
                override fun onInputBufferAvailable(codec: MediaCodec, index: Int) {}

                override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                    if (this@HwEncoder.codec !== codec) return
                    val buf = codec.getOutputBuffer(index)
                    val bytes = ByteArray(info.size)
                    if (buf != null && info.size > 0) {
                        buf.position(info.offset)
                        buf.get(bytes, 0, info.size)
                    }
                    codec.releaseOutputBuffer(index, false)
                    if (bytes.isEmpty()) return
                    if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                        val sp = H264.spsPps(bytes)
                        if (sp != null) onConfig(sp.first, sp.second) else onError("codec config without SPS/PPS")
                    } else {
                        onFrame(bytes, info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0)
                    }
                }

                override fun onError(codec: MediaCodec, e: MediaCodec.CodecException) {
                    if (this@HwEncoder.codec === codec) onError("encoder: ${e.diagnosticInfo}")
                }

                override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {}
            }, handler)
            c.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            surface = c.createInputSurface()
            c.start()
        } catch (e: Exception) {
            Log.w(TAG, "encoder setup", e)
            try { c.release() } catch (_: Exception) { }
            onError("encoder setup: ${e.message}")
            return false
        }
        codec = c
        description = "${c.name} ${w}x$h $fps fps ${bitRate / 1000} kbit/s" +
            if (f.containsKey(MediaFormat.KEY_BITRATE_MODE)) " CBR" else ""
        Log.i(TAG, "encoder: $description")
        return true
    }

    /** An IDR as soon as possible. */
    fun requestSync() {
        val c = codec ?: return
        try {
            c.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
        } catch (e: IllegalStateException) {
            Log.w(TAG, "request sync", e)
        }
    }

    fun release() {
        val c = codec ?: return
        codec = null
        try { c.stop() } catch (_: Exception) { }
        try { c.release() } catch (_: Exception) { }
        surface?.release()
        surface = null
    }

    companion object {
        private const val TAG = "AudiStreaming"
        // IDRs are big; the MMI needs one only at the start and after a
        // dropped frame (it asks with H264_STATUS SYNC)
        const val I_FRAME_INTERVAL_S = 10
    }
}
