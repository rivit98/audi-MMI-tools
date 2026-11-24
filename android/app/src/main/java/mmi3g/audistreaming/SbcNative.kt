package mmi3g.audistreaming

/**
 * BlueZ's SBC encoder (app/src/main/cpp/sbc_jni.cpp): 44.1 kHz, joint
 * stereo, 16 blocks, 8 subbands, loudness, the bitpool given. PCM in:
 * 16-bit little endian interleaved stereo, whole frames of [codeSize] bytes.
 * One instance per thread; [close] once.
 */
class SbcNative(bitpool: Int) : AutoCloseable {
    private var handle = open(bitpool)

    val ok: Boolean get() = handle != 0L
    /** PCM bytes per SBC frame (512). */
    val codeSize: Int get() = codeSize(handle)
    /** Bytes per SBC frame (119 at bitpool 53). */
    val frameLength: Int get() = frameLength(handle)

    /** Encodes pcm[0, n) (whole frames) into out; the bytes written, or -1. */
    fun encode(pcm: ByteArray, n: Int, out: ByteArray): Int = encode(handle, pcm, n, out)

    override fun close() {
        if (handle != 0L) close(handle)
        handle = 0L
    }

    private companion object {
        init { System.loadLibrary("sbc_jni") }

        @JvmStatic external fun open(bitpool: Int): Long
        @JvmStatic external fun codeSize(h: Long): Int
        @JvmStatic external fun frameLength(h: Long): Int
        @JvmStatic external fun encode(h: Long, pcm: ByteArray, n: Int, out: ByteArray): Int
        @JvmStatic external fun close(h: Long)
    }
}
