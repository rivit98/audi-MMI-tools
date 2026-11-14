package mmi3g.audistreaming

import android.content.Context
import android.graphics.SurfaceTexture
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.Surface
import android.view.TextureView

/**
 * The Android Auto mode's picture on the phone: Android Auto's whole video
 * in its own shape ([AaService.mode]), drawn by [GlScaler] into this view's
 * surface (call requestLayout after a new mode);
 * touches on it go to Android Auto ([AaService.touch]) and never move the
 * pages. The surface is handed over when it exists, and given back to the
 * system only once the GL thread has let go of it.
 */
class AaPreview @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null) :
    TextureView(context, attrs), TextureView.SurfaceTextureListener {
    private var surface: Surface? = null

    init {
        surfaceTextureListener = this
        isOpaque = true
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        val m = AaService.mode
        setMeasuredDimension(w, w * m.h / m.w)
    }

    override fun onSurfaceTextureAvailable(st: SurfaceTexture, width: Int, height: Int) {
        val s = Surface(st)
        surface = s
        AaService.setPreview(s, null)
    }

    override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, width: Int, height: Int) {}

    override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
        val s = surface
        surface = null
        AaService.setPreview(null) {
            s?.release()
            st.release()
        }
        return false                       // released above, once GL is done with it
    }

    override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}

    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (e.actionMasked == MotionEvent.ACTION_DOWN) parent?.requestDisallowInterceptTouchEvent(true)
        AaService.touch(e, width, height)
        return true
    }
}
