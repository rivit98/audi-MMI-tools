package mmi3g.audistreaming

import android.annotation.SuppressLint
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.projection.MediaProjection
import android.os.SystemClock

/**
 * The phone's own playback (music, video, games: what Android lets other
 * apps capture, USAGE_MEDIA / GAME / UNKNOWN) captured through the screen
 * projection, SBC-encoded ([SbcNative]) and sent to the MMI as AUDIO_*
 * (Proto). The MMI only checks and logs it for now; the phone keeps playing
 * the sound itself as well.
 *
 * One thread: blocking reads of ~50 ms of PCM, encoded into 17 frames, one
 * AUDIO_DATA each. It sends only while [Streaming.sendAudio] is on and the
 * link's MMI announced SBC (AUDIO_CAPS); a new link gets AUDIO_CONFIG first
 * and frame numbers from 0. Needs Android 10 (playback capture) and the
 * RECORD_AUDIO permission.
 */
class PhoneAudio(private val projection: MediaProjection) {
    @Volatile private var running = false
    private var thread: Thread? = null

    fun start() {
        if (running) return
        running = true
        thread = Thread(::run, "phone-audio").apply { isDaemon = true; start() }
    }

    fun stop() {
        running = false
        thread?.interrupt()
        thread = null
    }

    @SuppressLint("MissingPermission")   // checked by the caller (CaptureService)
    private fun record(): AudioRecord? = try {
        val capture = AudioPlaybackCaptureConfiguration.Builder(projection)
            .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
            .addMatchingUsage(AudioAttributes.USAGE_GAME)
            .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
            .build()
        val format = AudioFormat.Builder().setSampleRate(RATE)
            .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build()
        val min = AudioRecord.getMinBufferSize(RATE, AudioFormat.CHANNEL_IN_STEREO,
            AudioFormat.ENCODING_PCM_16BIT)
        AudioRecord.Builder().setAudioFormat(format)
            .setBufferSizeInBytes(maxOf(min, RATE * 4 / 2))       // ~0.5 s
            .setAudioPlaybackCaptureConfig(capture).build()
            .takeIf { it.state == AudioRecord.STATE_INITIALIZED }
    } catch (e: Exception) {
        AppLog.w("audio: no playback capture", e)
        null
    }

    private fun run() {
        val enc = SbcNative(BITPOOL)
        val rec = if (enc.ok) record() else null
        if (rec == null) {
            AppLog.w("audio: not started (encoder ${if (enc.ok) "ok" else "failed"})")
            enc.close()
            running = false
            return
        }
        val code = enc.codeSize
        val flen = enc.frameLength
        val pcm = ByteArray(code * FRAMES_PER_MSG)
        val out = ByteArray(flen * FRAMES_PER_MSG)
        var have = 0
        var link: AccessoryLink? = null
        var seq = 0
        var statusAt = SystemClock.elapsedRealtime()
        var sent = 0
        var dropped = 0
        var quiet = 0
        rec.startRecording()
        AppLog.i("audio: capturing $RATE Hz stereo, SBC bitpool $BITPOOL, $flen-byte frames, " +
            "$FRAMES_PER_MSG a message")
        try {
            while (running) {
                val n = rec.read(pcm, have, pcm.size - have, AudioRecord.READ_BLOCKING)
                if (n < 0) {
                    AppLog.w("audio: read failed ($n)")
                    break
                }
                have += n
                if (have < pcm.size) continue
                have = 0
                val now = SystemClock.elapsedRealtime()
                if (pcm.all { it.toInt() == 0 }) quiet++
                val l = Streaming.link
                val wanted = l != null && Streaming.sendAudio && l.audioCaps?.sbc == true
                if (!wanted || l !== link) {
                    link?.let {
                        it.sendAudio(Proto.encode(Proto.AUDIO_STOP, ByteArray(0)))
                        AppLog.i("audio: stopped on this link (sent $sent frames, dropped $dropped)")
                    }
                    link = null
                }
                if (!wanted) continue
                if (link == null) {
                    link = l
                    seq = 0
                    l!!.sendAudio(Proto.audioConfig(RATE, 2, 16, 8, MODE_JOINT, true, BITPOOL, flen))
                    AppLog.i("audio: AUDIO_CONFIG sent")
                }
                val bytes = enc.encode(pcm, pcm.size, out)
                if (bytes <= 0) {
                    AppLog.w("audio: encoder failed")
                    break
                }
                val frames = bytes / flen
                // the first sample of this message was captured frames x 128 samples ago
                val phoneMs = (now - frames * 128L * 1000 / RATE).toInt()
                if (link!!.sendAudio(Proto.audioData(seq, phoneMs, out, 0, bytes))) sent += frames
                else dropped += frames
                seq += frames
                if (now - statusAt >= STATUS_MS) {
                    AppLog.i("audio: sent $sent frames, dropped $dropped (link busy), " +
                        "$quiet silent reads of ${FRAMES_PER_MSG * 128 * 1000 / RATE} ms")
                    statusAt = now
                    quiet = 0
                }
            }
        } finally {
            link?.sendAudio(Proto.encode(Proto.AUDIO_STOP, ByteArray(0)))
            try { rec.stop() } catch (_: IllegalStateException) { }
            rec.release()
            enc.close()
            AppLog.i("audio: capture ended (sent $sent frames, dropped $dropped)")
        }
    }

    companion object {
        const val RATE = 44100
        const val BITPOOL = 53                 // A2DP "high quality": 119-byte frames, ~328 kbit/s
        const val FRAMES_PER_MSG = 17          // 17 x 119 + 8 = 2031 <= Proto.MAX_PAYLOAD; ~49 ms
        private const val MODE_JOINT = 3
        private const val STATUS_MS = 10_000L
    }
}
