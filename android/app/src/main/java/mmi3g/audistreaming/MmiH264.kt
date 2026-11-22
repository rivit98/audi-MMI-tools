package mmi3g.audistreaming

import android.os.SystemClock

/**
 * An H.264 encoder's output to the MMI's hardware stream, shared by both
 * modes (screen mirroring, [CaptureService]; Android Auto, [AaService]).
 * Confined to the owner's thread: the encoder's callbacks and [step] run
 * there.
 *
 * Every frame goes out in order - a lost one would spoil the ones after it
 * - so frames queue here while the MMI's acks are outstanding; if the queue
 * gets long, or a frame is bigger than the MMI's per-frame size, the frames
 * up to the next IDR are dropped and an IDR is asked for. A frame that
 * doesn't fit also raises the size (a new H264_CONFIG; the MMI restarts its
 * stream). The MMI's H264_STATUS: SYNC asks for an IDR, CONFIG for the
 * config again, REFUSED pauses H.264 on that link ([step] returns false;
 * [refused] says when to try again, [RefusedBackoff]).
 * After an H264_CONFIG no frame goes until the MMI says PLAYING (it would
 * only be refused while the MMI sets its stream up), and then the stream
 * starts at an IDR.
 */
class MmiH264 {
    var enc: HwEncoder? = null
        private set
    private var sps: ByteArray? = null
    private var pps: ByteArray? = null
    var frameBytes = START_FRAME_BYTES
        private set
    private val queue = ArrayDeque<ByteArray>()
    private var waitIdr = true
    private var configLink: AccessoryLink? = null     // the link the config went to
    /** The link that answered REFUSED, until H.264 is tried on it again. */
    var refusedLink: AccessoryLink? = null
        private set
    private var lastRefusedLink: AccessoryLink? = null
    private val backoff = RefusedBackoff()
    private var playingSeen = false                   // PLAYING since the last config
    private var nextId = 1
    val stats = Stats()
    val queued: Int get() = queue.size

    class Stats {
        var frames = 0; var bytes = 0L; var maxFrame = 0; var idrs = 0
        var dropped = 0; var tooBig = 0; var overflows = 0
        var windowAt = 0L; var windowBytes = 0L; var kbps = 0
    }

    /** A new encoder: its config comes, and the MMI restarts at it. The size
     * the frames needed stays: from 64 KB again, the next big IDR would cost
     * one more restart on the MMI. */
    fun newEncoder(e: HwEncoder, maxFrame: Int) {
        enc = e
        sps = null
        pps = null
        configLink = null
        queue.clear()
        waitIdr = true
        frameBytes = frameBytes.coerceAtMost(maxFrame)
    }

    /** The encoder is gone (released by the owner). */
    fun encoderGone() {
        enc = null
        sps = null
        pps = null
        configLink = null
        queue.clear()
    }

    /** One tick on [link]: the MMI's status, then what can go. False: the
     * MMI refused H.264 on this link ([refusedLink]); the owner stops. */
    fun step(link: AccessoryLink): Boolean {
        val encoder = enc ?: return true
        if (link.h264.refused.getAndSet(false)) {
            if (link !== lastRefusedLink) backoff.reset()
            lastRefusedLink = link
            refusedLink = link
            queue.clear()
            waitIdr = true
            val wait = backoff.refused(SystemClock.elapsedRealtime())
            AppLog.w("H.264: the MMI refused it: trying again in ${wait / 1000} s")
            return false
        }
        if (link !== configLink && sps != null) sendConfig(link, "new connection")
        if (link.h264.configWanted.getAndSet(false)) sendConfig(link, "the MMI asked")
        if (link.h264.sync.getAndSet(false)) encoder.requestSync()
        if (!playingSeen && link.h264.playing && configLink === link) {
            playingSeen = true
            // frames were held back until now: start at an IDR
            if (waitIdr) encoder.requestSync()
        }
        pump(link)
        val now = SystemClock.elapsedRealtime()
        if (now - stats.windowAt >= 1000) {
            stats.kbps = ((stats.bytes - stats.windowBytes) * 8 / maxOf(1L, now - stats.windowAt)).toInt()
            stats.windowAt = now
            stats.windowBytes = stats.bytes
        }
        return true
    }

    /** True while H.264 waits after the MMI's REFUSED on [link]. Once the
     * wait is over, the config goes again (if the encoder has one; else the
     * encoder's start sends it) and this says false. */
    fun refused(link: AccessoryLink): Boolean {
        if (link !== refusedLink) return false
        if (!backoff.due(SystemClock.elapsedRealtime())) return true
        refusedLink = null
        AppLog.i("H.264: trying again after the MMI's REFUSED (waited ${backoff.delayMs / 1000} s)")
        configLink = null
        if (enc != null) sendConfig(link, "trying again after REFUSED")
        return false
    }

    /** The status line's H.264 part. */
    fun describe(e: HwEncoder): String =
        "${e.fps} fps, ${stats.kbps} of ${e.bitRate / 1000} kbit/s, " +
            "frames ${stats.frames} (IDR ${stats.idrs}), max ${stats.maxFrame / 1024} of ${frameBytes / 1024} KB" +
            (if (queue.isNotEmpty()) ", queue ${queue.size}" else "") +
            (if (stats.dropped > 0) ", dropped ${stats.dropped} (too big ${stats.tooBig}, " +
                "queue full ${stats.overflows})" else "")

    private fun sendConfig(link: AccessoryLink, why: String) {
        val s = sps ?: return
        val p = pps ?: return
        val caps = link.videoCaps ?: return
        // PLAYING from before this config says nothing about it
        link.h264.playing = false
        link.sendH264Config(Proto.h264Config(caps.h264Fps, frameBytes, s, p))
        configLink = link
        // the MMI (re)starts its stream at the next IDR, after PLAYING
        playingSeen = false
        queue.clear()
        waitIdr = true
        enc?.requestSync()
        AppLog.i("H.264 config sent ($why): ${frameBytes / 1024} KB frames, SPS ${s.size} / PPS ${p.size} B")
    }

    /** The encoder's codec config (its callback). */
    fun onConfig(s: ByteArray, p: ByteArray) {
        sps = s
        pps = p
        Streaming.link?.let { sendConfig(it, "encoder started") }
    }

    /** One encoded frame (the encoder's callback). */
    fun onFrame(data: ByteArray, key: Boolean) {
        val link = Streaming.link
        if (link == null || configLink !== link || link === refusedLink) return      // the MMI has no stream for it
        if (!playingSeen) {
            // the MMI is still setting its stream up: it would refuse it
            stats.dropped++
            waitIdr = true
            return
        }
        val idr = key && H264.isIdr(data)
        if (waitIdr && !idr) {
            stats.dropped++
            return
        }
        // an encoder that repeats SPS/PPS before its IDRs, but not the
        // csd's bytes: the MMI refuses in-band ones that differ from its
        // config (and would ask for the same config again forever), so the
        // in-band ones become the config
        if (idr) {
            val inBand = H264.spsPps(data)
            val s = sps
            val p = pps
            if (inBand != null && s != null && p != null &&
                (!inBand.first.contentEquals(s) || !inBand.second.contentEquals(p))) {
                sps = inBand.first
                pps = inBand.second
                stats.dropped++
                sendConfig(link, "in-band SPS/PPS differ from the codec config")
                return
            }
        }
        val need = H264.sampleBytes(data)
        if (need > frameBytes) {
            stats.dropped++
            stats.tooBig++
            val max = link.videoCaps?.h264MaxFrame ?: frameBytes
            val grown = H264.grow(frameBytes, need, max)
            AppLog.w("H.264 frame of $need B > $frameBytes B${if (grown > frameBytes) ": now $grown" else ""}")
            if (grown > frameBytes) {
                frameBytes = grown
                sendConfig(link, "bigger frames")
            } else {
                waitIdr = true
                enc?.requestSync()
            }
            return
        }
        if (queue.size >= MAX_QUEUE) {
            stats.dropped += queue.size + 1
            stats.overflows++
            queue.clear()
            waitIdr = true
            enc?.requestSync()
            return
        }
        waitIdr = false
        queue.addLast(data)
        stats.frames++
        stats.bytes += data.size
        if (idr) stats.idrs++
        if (need > stats.maxFrame) stats.maxFrame = need
        pump(link)
    }

    private fun pump(link: AccessoryLink) {
        val e = enc ?: return
        while (queue.isNotEmpty() && link.readyForFrame()) {
            val f = queue.first()
            if (!link.sendFrame(nextId, e.w, e.h, f)) break
            queue.removeFirst()
            nextId = if (nextId == 0xffff) 1 else nextId + 1
        }
    }

    companion object {
        const val START_FRAME_BYTES = 64 * 1024
        private const val MAX_QUEUE = 25  // ~1 s at 25 fps not acked: start again at an IDR
    }
}
