package mmi3g.audistreaming

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack

/**
 * Android Auto's PCM on the phone's own audio (to the car over Bluetooth,
 * like any phone sound): one AudioTrack per channel, made on its first
 * data. The Android Auto mode advertises system sounds only (the phone
 * plays music and navigation itself), but media and guidance are handled
 * too. Writes never block the session thread: what doesn't fit is dropped.
 */
class AaAudio {
    private val tracks = HashMap<Int, AudioTrack?>()
    @Volatile var droppedBytes = 0L
        private set

    fun play(channel: Int, data: ByteArray) {
        val t = tracks.getOrPut(channel) { make(channel) } ?: return
        val n = t.write(data, 0, data.size, AudioTrack.WRITE_NON_BLOCKING)
        if (n < data.size) droppedBytes += data.size - maxOf(n, 0)
    }

    private fun make(channel: Int): AudioTrack? {
        val stereo = channel == AaNative.CH_MEDIA_AUDIO
        val rate = if (stereo) 48000 else 16000
        val mask = if (stereo) AudioFormat.CHANNEL_OUT_STEREO else AudioFormat.CHANNEL_OUT_MONO
        val (usage, content) = when (channel) {
            AaNative.CH_MEDIA_AUDIO -> AudioAttributes.USAGE_MEDIA to AudioAttributes.CONTENT_TYPE_MUSIC
            AaNative.CH_GUIDANCE_AUDIO ->
                AudioAttributes.USAGE_ASSISTANCE_NAVIGATION_GUIDANCE to AudioAttributes.CONTENT_TYPE_SPEECH
            else -> AudioAttributes.USAGE_ASSISTANCE_SONIFICATION to AudioAttributes.CONTENT_TYPE_SONIFICATION
        }
        val min = AudioTrack.getMinBufferSize(rate, mask, AudioFormat.ENCODING_PCM_16BIT)
        // ~300 ms: the phone sends in bursts
        val size = maxOf(min * 2, rate * (if (stereo) 4 else 2) * 3 / 10)
        return try {
            AudioTrack.Builder()
                .setAudioAttributes(AudioAttributes.Builder().setUsage(usage)
                    .setContentType(content).build())
                .setAudioFormat(AudioFormat.Builder().setSampleRate(rate).setChannelMask(mask)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                .setBufferSizeInBytes(size)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
                .apply { play() }
        } catch (e: Exception) {
            AppLog.w("AA audio channel $channel", e)
            null
        }
    }

    fun release() {
        for (t in tracks.values) {
            try { t?.stop() } catch (_: Exception) { }
            t?.release()
        }
        tracks.clear()
    }
}
