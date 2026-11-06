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
 * Between the virtual display and the H.264 encoder: the screen is
 * captured at k times the picture into [inputSurface] (a SurfaceTexture),
 * and every new capture frame is box-filtered k:1 ([GlBox]) into the
 * encoder's input surface, with the capture's timestamp, on its own GL
 * thread. The encoder sees an ordinary producer: its rate limit and
 * repeat-previous-frame work as before.
 *
 * [start] and [release] block until the GL thread has done its part, so
 * after [release] nothing draws into the encoder's surface any more.
 */
class GlScaler(
    private val target: Surface,
    val w: Int,
    val h: Int,
    val k: Int,
) {
    val srcW = w * k
    val srcH = h * k
    var inputSurface: Surface? = null
        private set
    @Volatile var frames = 0
        private set
    @Volatile var error = ""
        private set

    private var thread: HandlerThread? = null
    private var handler: Handler? = null
    private var display: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var context: EGLContext = EGL14.EGL_NO_CONTEXT
    private var surface: EGLSurface = EGL14.EGL_NO_SURFACE
    private var texture: SurfaceTexture? = null
    private var tex = 0
    private var program = 0
    private var aPos = 0
    private var aTex = 0
    private var uTexMatrix = 0
    private var uStepX = 0
    private var uStepY = 0
    private val matrix = FloatArray(16)
    @Volatile private var dead = false

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
        else Log.i(TAG, "GL scaler: ${srcW}x$srcH -> ${w}x$h (x$k, ${GlBox.taps(k).size} taps, " +
            "${if (GlBox.linear(k)) "bilinear" else "nearest"})")
        return ok
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
        context = EGL14.eglCreateContext(display, config, EGL14.EGL_NO_CONTEXT,
            intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0)
        if (context == EGL14.EGL_NO_CONTEXT) fail("eglCreateContext")
        surface = EGL14.eglCreateWindowSurface(display, config, target, intArrayOf(EGL14.EGL_NONE), 0)
        if (surface == EGL14.EGL_NO_SURFACE) fail("eglCreateWindowSurface")
        if (!EGL14.eglMakeCurrent(display, surface, surface, context)) fail("eglMakeCurrent")

        program = buildProgram(GlBox.VERTEX_SHADER, GlBox.fragmentShader(k))
        aPos = GLES20.glGetAttribLocation(program, "aPos")
        aTex = GLES20.glGetAttribLocation(program, "aTex")
        uTexMatrix = GLES20.glGetUniformLocation(program, "uTexMatrix")
        uStepX = GLES20.glGetUniformLocation(program, "uStepX")
        uStepY = GLES20.glGetUniformLocation(program, "uStepY")

        val ids = IntArray(1)
        GLES20.glGenTextures(1, ids, 0)
        tex = ids[0]
        val filter = if (GlBox.linear(k)) GLES20.GL_LINEAR else GLES20.GL_NEAREST
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, tex)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, filter)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, filter)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        checkGl("texture")

        val st = SurfaceTexture(tex)
        st.setDefaultBufferSize(srcW, srcH)
        st.setOnFrameAvailableListener({ draw() }, hd)
        texture = st
        inputSurface = Surface(st)
    }

    private fun draw() {
        val st = texture ?: return
        if (dead) return
        try {
            st.updateTexImage()
            st.getTransformMatrix(matrix)
            GLES20.glViewport(0, 0, w, h)
            GLES20.glUseProgram(program)
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, tex)
            GLES20.glUniformMatrix4fv(uTexMatrix, 1, false, matrix, 0)
            val s = GlBox.steps(matrix, srcW, srcH)
            GLES20.glUniform2f(uStepX, s[0], s[1])
            GLES20.glUniform2f(uStepY, s[2], s[3])
            GLES20.glEnableVertexAttribArray(aPos)
            GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 0, QUAD_POS)
            GLES20.glEnableVertexAttribArray(aTex)
            GLES20.glVertexAttribPointer(aTex, 2, GLES20.GL_FLOAT, false, 0, QUAD_TEX)
            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
            EGLExt.eglPresentationTimeANDROID(display, surface, st.timestamp)
            if (!EGL14.eglSwapBuffers(display, surface)) fail("eglSwapBuffers")
            frames++
        } catch (e: RuntimeException) {
            // the encoder's surface went away under us (stopping): stop drawing
            Log.w(TAG, "GL scaler draw", e)
            error = e.message ?: e.toString()
            dead = true
        }
    }

    private fun tearDown() {
        dead = true
        texture?.setOnFrameAvailableListener(null)
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

        // full-screen quad, triangle strip; texture coordinates 0..1 before
        // the SurfaceTexture's transform
        private val QUAD_POS = floats(-1f, -1f, 1f, -1f, -1f, 1f, 1f, 1f)
        private val QUAD_TEX = floats(0f, 0f, 1f, 0f, 0f, 1f, 1f, 1f)
    }
}
