package mmi3g.audistreaming

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface

/**
 * What the MMI shows instead of the phone's screen while H.264 can't
 * stream it: a portrait screen in a landscape stream only fills a narrow
 * strip down the middle of the MMI (210x432 of 720x432) with unreadable
 * text, so it isn't streamed at all. Drawn at the encoder's size; the MMI
 * scales it to 800x480 like any frame.
 */
object Notice {
    fun landscapeRequired(w: Int, h: Int): Bitmap =
        card(w, h, "Turn the phone to landscape", "The MMI shows the phone's screen in landscape only")

    private fun card(w: Int, h: Int, title: String, line: String): Bitmap {
        val b = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val c = Canvas(b)
        c.drawColor(Color.BLACK)
        val big = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            textAlign = Paint.Align.CENTER
            typeface = Typeface.DEFAULT_BOLD
            textSize = h / 9f
        }
        val small = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.rgb(0xb0, 0xb0, 0xb0)
            textAlign = Paint.Align.CENTER
            textSize = h / 17f
        }
        fit(big, title, w * 0.9f)
        fit(small, line, w * 0.9f)
        // a phone turned on its side, then the two lines
        val cx = w / 2f
        val icon = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            style = Paint.Style.STROKE
            strokeWidth = h / 90f
        }
        val iw = h * 0.30f
        val ih = h * 0.17f
        val iy = h * 0.30f
        c.drawRoundRect(cx - iw / 2, iy - ih / 2, cx + iw / 2, iy + ih / 2, ih / 8, ih / 8, icon)
        c.drawText(title, cx, h * 0.62f, big)
        c.drawText(line, cx, h * 0.77f, small)
        return b
    }

    private fun fit(p: Paint, text: String, maxW: Float) {
        while (p.measureText(text) > maxW && p.textSize > 8f) p.textSize *= 0.95f
    }
}
