package rivit.usbtx

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ProtoTest {
    private data class Frame(val type: Int, val payload: ByteArray)

    private fun decodeAll(dec: Proto.Decoder, bytes: ByteArray, chunk: Int): List<Frame> {
        val out = mutableListOf<Frame>()
        var i = 0
        while (i < bytes.size) {
            val n = minOf(chunk, bytes.size - i)
            dec.feed(bytes, i, n) { t, p -> out += Frame(t, p) }
            i += n
        }
        return out
    }

    @Test
    fun encodeMatchesTheCLayout() {
        // type u8 | len u16 LE | payload  (src/usb_tx_test/proto.h)
        assertArrayEquals(
            byteArrayOf(2, 4, 0, 5, 0, 'h'.code.toByte(), 'i'.code.toByte()),
            Proto.encode(Proto.TEXT, byteArrayOf(5, 0, 'h'.code.toByte(), 'i'.code.toByte())),
        )
        assertArrayEquals(byteArrayOf(3, 0, 0), Proto.encode(Proto.ACK, ByteArray(0)))
    }

    @Test
    fun textFrameCarriesSeqAndUtf8() {
        val f = Proto.text(0x0102, "zażółć")
        val got = decodeAll(Proto.Decoder(), f, 1)
        assertEquals(1, got.size)
        assertEquals(Proto.TEXT, got[0].type)
        assertEquals(0x0102, Proto.u16(got[0].payload, 0))
        assertEquals("zażółć", String(got[0].payload, 2, got[0].payload.size - 2, Charsets.UTF_8))
    }

    @Test
    fun textTooLongIsRejected() {
        assertNull(Proto.textOrNull(1, "x".repeat(Proto.MAX_TEXT_BYTES + 1)))
        assertEquals(Proto.HDR + Proto.MAX_PAYLOAD, Proto.textOrNull(1, "x".repeat(Proto.MAX_TEXT_BYTES))!!.size)
        // a 2-byte character that would cross the limit
        assertNull(Proto.textOrNull(1, "x".repeat(Proto.MAX_TEXT_BYTES - 1) + "ż"))
    }

    @Test
    fun mergedAndSplitFrames() {
        val bytes = Proto.encode(Proto.HELLO, "mmi".toByteArray()) +
            Proto.encode(Proto.ACK, byteArrayOf(7, 0)) +
            Proto.encode(Proto.HELLO_REPLY, ByteArray(0))
        for (chunk in listOf(1, 2, 5, bytes.size)) {
            val got = decodeAll(Proto.Decoder(), bytes, chunk)
            assertEquals(listOf(Proto.HELLO, Proto.ACK, Proto.HELLO_REPLY), got.map { it.type })
            assertEquals("mmi", String(got[0].payload))
            assertEquals(7, Proto.u16(got[1].payload, 0))
            assertEquals(0, got[2].payload.size)
        }
    }

    @Test
    fun oversizedFrameIsSkipped() {
        val big = Proto.MAX_PAYLOAD + 10
        val bad = byteArrayOf(Proto.TEXT.toByte(), (big and 0xff).toByte(), (big shr 8).toByte()) +
            ByteArray(big) { 0x55 }
        val dec = Proto.Decoder()
        val got = decodeAll(dec, bad + Proto.encode(Proto.ACK, byteArrayOf(9, 0)), 700)
        assertEquals(1, dec.oversized)
        assertEquals(listOf(Proto.ACK), got.map { it.type })
        assertEquals(9, Proto.u16(got[0].payload, 0))
    }
}
