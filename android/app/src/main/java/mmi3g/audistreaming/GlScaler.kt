package mmi3g.audistreaming

import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLExt
import android.opengl.EGLSurface
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * Between Android Auto's H.264 decoder ([AaDecoder]) and the encoder for
 * the MMI: the decoder draws into [inputSurface] (a SurfaceTexture), and on
 * every tick of a [FrameClock] (exactly fps a second) the newest frame's
 * kept part ([FrameGeometry]) goes into the middle of the encoder's input
 * surface, black around it, with the tick's time, on its own GL thread:
 * copied 1:1, or scaled bilinear ([GlBox]). A still picture is drawn again
 * on every tick, so the encoder gets exactly fps frames a second - more
 * would pile up as lag on the MMI, which plays exactly 25.
 *
 * [start] and [release] block until the GL thread has done its part, so
 * after [release] nothing draws into the encoder's surface any more.
 *
 * [setPreview] also draws each frame's picture into a phone-side surface
 * (the touch preview), stretched to it and bilinear-filtered. [retarget]
 * moves the drawing to a new encoder (another rate) while the input stays:
 * the decoder goes on undisturbed.
 */
class GlScaler(
    private var target: Surface,
    val w: Int,
    val h: Int,
    start: FrameGeometry,
    fps: Int,
) {
    @Volatile var fps = fps
        private set
    @Volatile var geometry = start
        private set
    var inputSurface: Surface? = null
        private set
    /** Frames drawn into the encoder (one per tick). */
    @Volatile var drawn = 0
        private set
    @Volatile var error = ""
        private set

    private var thread: HandlerThread? = null
    private var handler: Handler? = null
    private var display: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var context: EGLContext = EGL14.EGL_NO_CONTEXT
    private var surface: EGLSurface = EGL14.EGL_NO_SURFACE
    private var eglConfig: EGLConfig? = null
    // the preview (GL thread)
    private var preview: EGLSurface = EGL14.EGL_NO_SURFACE
    /** Frames drawn into the preview. */
    @Volatile var previewDrawn = 0
        private set
    private var texture: SurfaceTexture? = null
    private var tex = 0
    private var program = 0
    private var quadTex = floats(*GlBox.quadTex(start))
    private var aPos = 0
    private var aTex = 0
    private var uTexMatrix = 0
    private var uStepX = 0
    private var uStepY = 0
    private val matrix = FloatArray(16)
    @Volatile private var dead = false
    private var clock: FrameClock? = null
    private var haveFrame = false
    private val tick = Runnable { onTick() }

    /** Drawing failed (after [start] succeeded): the encoder gets no new
     * frames, so the owner should start again without the scaler. */
    val failed: Boolean get() = dead && thread != null

    /** Set up on the GL thread; false (and [error]) if the phone can't. */
    fun start(): Boolean {
        val t = HandlerThread("gl-scaler").apply { start() }
        thread = t
        val hd = Handler(t.looper)
        handler = hd
        val done = CountDownLatch(1)
        var ok = false
        hd.post {
            ok = try {
                setUp(hd)
                true
            } catch (e: RuntimeException) {
                Log.w(TAG, "GL scaler setup", e)
                error = e.message ?: e.toString()
                tearDown()
                false
            }
            done.countDown()
        }
        if (!done.await(2, TimeUnit.SECONDS)) {
            error = "GL setup timed out"
            ok = false
        }
        if (!ok) quit()
        else Log.i(TAG, "GL scaler: $geometry -> ${w}x$h")
        return ok
    }
    /** The preview shows the whole input frame, not only the kept part:
     * the rail the MMI's picture leaves out stays touchable on the phone. */
    @Volatile var previewWhole = false

    /** Draws every frame's picture into [s] too, from the next tick on;
     * null: no more (blocks until the GL thread has let go of the old one,
     * so a TextureView may destroy it after this returns). False if the
     * preview's EGL surface couldn't be made; the encoder is not affected. */
    fun setPreview(s: Surface?): Boolean {
        val hd = handler ?: return false
        val done = CountDownLatch(1)
        var ok = false
        hd.post {
            dropPreview()
            ok = s == null
            if (s != null && display != EGL14.EGL_NO_DISPLAY) {
                val p = EGL14.eglCreateWindowSurface(display, eglConfig, s, intArrayOf(EGL14.EGL_NONE), 0)
                if (p != EGL14.EGL_NO_SURFACE) {
                    preview = p
                    ok = true
                } else {
                    Log.w(TAG, "preview surface: EGL 0x${Integer.toHexString(EGL14.eglGetError())}")
                }
            }
            done.countDown()
        }
        return done.await(2, TimeUnit.SECONDS) && ok
    }

    /** Draws into [s] (a new encoder's input, w x h) at [newFps] from the
     * next tick on; the old target is let go of before this returns, so its
     * encoder may be released after. False (and [failed]) if the GL thread
     * couldn't. */
    fun retarget(s: Surface, newFps: Int): Boolean {
        val hd = handler ?: return false
        val done = CountDownLatch(1)
        hd.post {
            try {
                val old = surface
                val n = EGL14.eglCreateWindowSurface(display, eglConfig, s, intArrayOf(EGL14.EGL_NONE), 0)
                if (n == EGL14.EGL_NO_SURFACE) fail("eglCreateWindowSurface")
                if (!EGL14.eglMakeCurrent(display, n, n, context)) {
                    EGL14.eglDestroySurface(display, n)
                    fail("eglMakeCurrent")
                }
                surface = n
                target = s
                if (old != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, old)
                if (newFps != fps) {
                    fps = newFps
                    clock = FrameClock(newFps, System.nanoTime())
                    hd.removeCallbacks(tick)
                    hd.post(tick)
                }
            } catch (e: RuntimeException) {
                Log.w(TAG, "GL scaler retarget", e)
                error = e.message ?: e.toString()
                dead = true
            }
            done.countDown()
        }
        return done.await(2, TimeUnit.SECONDS) && !dead
    }

    fun release() {
        val hd = handler ?: return
        val done = CountDownLatch(1)
        hd.post {
            tearDown()
            done.countDown()
        }
        done.await(2, TimeUnit.SECONDS)
        quit()
    }

    private fun quit() {
        thread?.quitSafely()
        thread = null
        handler = null
    }

    // --- GL thread ------------------------------------------------------------

    private fun setUp(hd: Handler) {
        display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        if (display == EGL14.EGL_NO_DISPLAY) fail("eglGetDisplay")
        val version = IntArray(2)
        if (!EGL14.eglInitialize(display, version, 0, version, 1)) fail("eglInitialize")
        val attribs = intArrayOf(
            EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            EGL_RECORDABLE_ANDROID, 1,
            EGL14.EGL_NONE,
        )
        val configs = arrayOfNulls<EGLConfig>(1)
        val n = IntArray(1)
        if (!EGL14.eglChooseConfig(display, attribs, 0, configs, 0, 1, n, 0) || n[0] < 1) fail("eglChooseConfig")
        val config = configs[0]!!
        eglConfig = config
        context = EGL14.eglCreateContext(display, config, EGL14.EGL_NO_CONTEXT,
            intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0)
        if (context == EGL14.EGL_NO_CONTEXT) fail("eglCreateContext")
        surface = EGL14.eglCreateWindowSurface(display, config, target, intArrayOf(EGL14.EGL_NONE), 0)
        if (surface == EGL14.EGL_NO_SURFACE) fail("eglCreateWindowSurface")
        if (!EGL14.eglMakeCurrent(display, surface, surface, context)) fail("eglMakeCurrent")

        newInput(geometry.capW, geometry.capH, hd)
        useFactor(geometry)

        clock = FrameClock(fps, System.nanoTime())
        hd.post(tick)
    }

    /** A new texture and the input surface into it, w x h. */
    private fun newInput(w: Int, h: Int, hd: Handler) {
        val ids = IntArray(1)
        GLES20.glGenTextures(1, ids, 0)
        tex = ids[0]
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, tex)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        checkGl("texture")
        val st = SurfaceTexture(tex)
        st.setDefaultBufferSize(w, h)
        st.setOnFrameAvailableListener({ latch() }, hd)
        texture = st
        inputSurface = Surface(st)
    }

    /** The shader depends on k, the texture's filter on k and scaling. */
    private fun useFactor(g: FrameGeometry) {
        val p = buildProgram(GlBox.VERTEX_SHADER, GlBox.fragmentShader(g))
        if (program != 0) GLES20.glDeleteProgram(program)
        program = p
        aPos = GLES20.glGetAttribLocation(p, "aPos")
        aTex = GLES20.glGetAttribLocation(p, "aTex")
        uTexMatrix = GLES20.glGetUniformLocation(p, "uTexMatrix")
        uStepX = GLES20.glGetUniformLocation(p, "uStepX")
        uStepY = GLES20.glGetUniformLocation(p, "uStepY")
        setFilter(if (GlBox.linear(g)) GLES20.GL_LINEAR else GLES20.GL_NEAREST)
    }

    /** A new decoded frame: latched now (the decoder mustn't wait
     * for its buffers), drawn on the next tick. */
    private fun latch() {
        val st = texture ?: return
        if (dead) return
        try {
            st.updateTexImage()
            st.getTransformMatrix(matrix)
            haveFrame = true
        } catch (e: RuntimeException) {
            Log.w(TAG, "GL scaler latch", e)
            error = e.message ?: e.toString()
            dead = true
        }
    }

    private fun onTick() {
        val c = clock ?: return
        val hd = handler ?: return
        if (dead) return
        val t = c.take(System.nanoTime())
        if (t >= 0 && haveFrame) draw(t)
        if (!dead) hd.postDelayed(tick, c.delayMs(System.nanoTime()))
    }

    private fun draw(ptsNs: Long) {
        try {
            GLES20.glClearColor(0f, 0f, 0f, 1f)
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
            drawScreen()
            EGLExt.eglPresentationTimeANDROID(display, surface, ptsNs)
            if (!EGL14.eglSwapBuffers(display, surface)) fail("eglSwapBuffers")
            drawn++
            if (preview != EGL14.EGL_NO_SURFACE) drawPreview()
        } catch (e: RuntimeException) {
            // the encoder's surface went away under us (stopping): stop drawing
            Log.w(TAG, "GL scaler draw", e)
            error = e.message ?: e.toString()
            dead = true
        }
    }

    private fun drawScreen() {
        // the encoder's buffers keep old content: black around the picture
        val g = geometry
        drawPicture((w - g.picW) / 2, (h - g.picH) / 2, g.picW, g.picH)
    }

    /** The picture again into the preview, stretched, bilinear; a preview
     * that fails is dropped (its view went away), the encoder goes on. */
    private fun drawPreview() {
        val size = IntArray(2)
        if (!EGL14.eglMakeCurrent(display, preview, preview, context) ||
            !EGL14.eglQuerySurface(display, preview, EGL14.EGL_WIDTH, size, 0) ||
            !EGL14.eglQuerySurface(display, preview, EGL14.EGL_HEIGHT, size, 1)) {
            Log.w(TAG, "preview: EGL 0x${Integer.toHexString(EGL14.eglGetError())}, dropped")
            dropPreview()
            return
        }
        EGL14.eglSwapInterval(display, 0)
        GLES20.glClearColor(0f, 0f, 0f, 1f)
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
        setFilter(GLES20.GL_LINEAR)
        drawPicture(0, 0, size[0], size[1], if (previewWhole) WHOLE_TEX else quadTex)
        setFilter(if (GlBox.linear(geometry)) GLES20.GL_LINEAR else GLES20.GL_NEAREST)
        if (EGL14.eglSwapBuffers(display, preview)) previewDrawn++
        else {
            Log.w(TAG, "preview swap: EGL 0x${Integer.toHexString(EGL14.eglGetError())}, dropped")
            dropPreview()
        }
        if (!EGL14.eglMakeCurrent(display, surface, surface, context)) fail("eglMakeCurrent")
    }

    private fun dropPreview() {
        if (preview == EGL14.EGL_NO_SURFACE) return
        if (display != EGL14.EGL_NO_DISPLAY) {
            EGL14.eglMakeCurrent(display, surface, surface, context)
            EGL14.eglDestroySurface(display, preview)
        }
        preview = EGL14.EGL_NO_SURFACE
    }

    private fun setFilter(filter: Int) {
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, tex)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, filter)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, filter)
    }

    private fun drawPicture(x: Int, y: Int, pw: Int, ph: Int, texCoords: FloatBuffer = quadTex) {
        val g = geometry
        GLES20.glViewport(x, y, pw, ph)
        GLES20.glUseProgram(program)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, tex)
        GLES20.glUniformMatrix4fv(uTexMatrix, 1, false, matrix, 0)
        val s = GlBox.steps(matrix, g.capW, g.capH)
        GLES20.glUniform2f(uStepX, s[0], s[1])
        GLES20.glUniform2f(uStepY, s[2], s[3])
        GLES20.glEnableVertexAttribArray(aPos)
        GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 0, QUAD_POS)
        GLES20.glEnableVertexAttribArray(aTex)
        GLES20.glVertexAttribPointer(aTex, 2, GLES20.GL_FLOAT, false, 0, texCoords)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
    }

    private fun tearDown() {
        dead = true
        handler?.removeCallbacks(tick)
        texture?.setOnFrameAvailableListener(null)
        dropPreview()
        inputSurface?.release()
        inputSurface = null
        texture?.release()
        texture = null
        if (display != EGL14.EGL_NO_DISPLAY) {
            if (program != 0) GLES20.glDeleteProgram(program)
            if (tex != 0) GLES20.glDeleteTextures(1, intArrayOf(tex), 0)
            program = 0
            tex = 0
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
            if (surface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, surface)
            if (context != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(display, context)
            // no eglTerminate: the display is the process's, shared with the UI
            EGL14.eglReleaseThread()
        }
        surface = EGL14.EGL_NO_SURFACE
        context = EGL14.EGL_NO_CONTEXT
        display = EGL14.EGL_NO_DISPLAY
    }

    private fun buildProgram(vs: String, fs: String): Int {
        val v = compile(GLES20.GL_VERTEX_SHADER, vs)
        val f = compile(GLES20.GL_FRAGMENT_SHADER, fs)
        val p = GLES20.glCreateProgram()
        GLES20.glAttachShader(p, v)
        GLES20.glAttachShader(p, f)
        GLES20.glLinkProgram(p)
        val st = IntArray(1)
        GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, st, 0)
        GLES20.glDeleteShader(v)
        GLES20.glDeleteShader(f)
        if (st[0] == 0) {
            val log = GLES20.glGetProgramInfoLog(p)
            GLES20.glDeleteProgram(p)
            fail("link: $log")
        }
        return p
    }

    private fun compile(type: Int, src: String): Int {
        val s = GLES20.glCreateShader(type)
        GLES20.glShaderSource(s, src)
        GLES20.glCompileShader(s)
        val st = IntArray(1)
        GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, st, 0)
        if (st[0] == 0) {
            val log = GLES20.glGetShaderInfoLog(s)
            GLES20.glDeleteShader(s)
            fail("${if (type == GLES20.GL_VERTEX_SHADER) "vertex" else "fragment"} shader: $log")
        }
        return s
    }

    private fun checkGl(what: String) {
        val e = GLES20.glGetError()
        if (e != GLES20.GL_NO_ERROR) fail("$what: GL error 0x${Integer.toHexString(e)}")
    }

    private fun fail(what: String): Nothing {
        val egl = EGL14.eglGetError()
        throw RuntimeException(if (egl != EGL14.EGL_SUCCESS) "$what (EGL 0x${Integer.toHexString(egl)})" else what)
    }

    companion object {
        private const val TAG = "AudiStreaming"
        private const val EGL_RECORDABLE_ANDROID = 0x3142

        private fun floats(vararg v: Float): FloatBuffer =
            ByteBuffer.allocateDirect(v.size * 4).order(ByteOrder.nativeOrder()).asFloatBuffer().apply {
                put(v)
                position(0)
            }

        // full-viewport quad, triangle strip; its texture coordinates are
        // GlBox.quadTex
        private val QUAD_POS = floats(-1f, -1f, 1f, -1f, -1f, 1f, 1f, 1f)
        // the whole input frame, in GlBox.quadTex's order
        private val WHOLE_TEX = floats(0f, 0f, 1f, 0f, 0f, 1f, 1f, 1f)
    }
}
