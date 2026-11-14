package mmi3g.audistreaming

import android.content.Context
import android.graphics.Rect
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import android.view.VelocityTracker
import android.view.ViewConfiguration
import android.view.ViewGroup
import android.widget.AbsSeekBar
import android.widget.OverScroller
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * Pages side by side, one screen wide each, switched by sliding them
 * sideways (or [setPage]): the app's two modes, screen mirroring and
 * Android Auto (MainActivity). A horizontal drag takes over from the page
 * once it is longer than the touch slop and more sideways than up or down,
 * so the pages still scroll vertically; a child that wants every touch
 * (the Android Auto preview) calls requestDisallowInterceptTouchEvent, and
 * a drag that starts on a slider (the fps slider) is the slider's. A
 * fling or a drag past half a page goes to the next page. [onPage] runs
 * when a different page has settled.
 *
 * Not ViewPager: the app has no AndroidX.
 */
class SwipePager @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null) : ViewGroup(context, attrs) {
    var onPage: ((Int) -> Unit)? = null
    var page = 0
        private set

    private val scroller = OverScroller(context)
    private val slop = ViewConfiguration.get(context).scaledTouchSlop
    private val minFling = ViewConfiguration.get(context).scaledMinimumFlingVelocity
    private var velocity: VelocityTracker? = null
    private var downX = 0f
    private var downY = 0f
    private var lastX = 0f
    private var dragging = false
    private var onSlider = false
    private val hit = Rect()

    /** Goes to page [p]; [onPage] runs if it is a different one. */
    fun setPage(p: Int, animate: Boolean = true) {
        val target = p.coerceIn(0, maxOf(0, childCount - 1))
        val dx = target * width - scrollX
        if (animate && width > 0) {
            scroller.forceFinished(true)
            scroller.startScroll(scrollX, 0, dx, 0, 300)
            postInvalidateOnAnimation()
        } else {
            scroller.forceFinished(true)
            scrollTo(target * width, 0)
        }
        if (target != page) {
            page = target
            onPage?.invoke(target)
        }
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        val h = MeasureSpec.getSize(heightMeasureSpec)
        val cw = MeasureSpec.makeMeasureSpec(w, MeasureSpec.EXACTLY)
        val ch = MeasureSpec.makeMeasureSpec(h, MeasureSpec.EXACTLY)
        for (i in 0 until childCount) getChildAt(i).measure(cw, ch)
        setMeasuredDimension(w, h)
    }

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        val w = r - l
        for (i in 0 until childCount) getChildAt(i).layout(i * w, 0, (i + 1) * w, b - t)
        if (scroller.isFinished) scrollTo(page * w, 0)   // a rotation keeps the page
    }

    override fun computeScroll() {
        if (scroller.computeScrollOffset()) {
            scrollTo(scroller.currX, 0)
            postInvalidateOnAnimation()
        }
    }

    override fun onInterceptTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = e.x
                downY = e.y
                lastX = e.x
                dragging = !scroller.isFinished
                if (dragging) scroller.forceFinished(true)
                onSlider = !dragging && sliderAt(this, e.rawX.toInt(), e.rawY.toInt())
                track(e, reset = true)
            }
            MotionEvent.ACTION_MOVE -> {
                track(e)
                if (onSlider) return false
                val dx = e.x - downX
                val dy = e.y - downY
                if (abs(dx) > slop && abs(dx) > abs(dy) * 1.5f) {
                    dragging = true
                    lastX = e.x
                    parent?.requestDisallowInterceptTouchEvent(true)
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> dragging = false
        }
        return dragging
    }

    override fun onTouchEvent(e: MotionEvent): Boolean {
        track(e)
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = e.x
                lastX = e.x
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = (lastX - e.x).roundToInt()
                lastX = e.x
                val max = (childCount - 1) * width
                scrollTo((scrollX + dx).coerceIn(0, max), 0)
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                val v = velocity
                v?.computeCurrentVelocity(1000)
                val vx = v?.xVelocity ?: 0f
                val from = page
                val target = when {
                    vx < -minFling * 4 -> from + 1
                    vx > minFling * 4 -> from - 1
                    else -> ((scrollX + width / 2) / maxOf(1, width))
                }
                dragging = false
                velocity?.recycle()
                velocity = null
                setPage(target)                 // the same page: back into place
            }
        }
        return true
    }

    /** Whether a shown slider under v is at screen point (x, y). */
    private fun sliderAt(v: View, x: Int, y: Int): Boolean {
        if (v.visibility != View.VISIBLE) return false
        if (v is AbsSeekBar) return v.isEnabled && v.getGlobalVisibleRect(hit) && hit.contains(x, y)
        if (v !is ViewGroup) return false
        return (0 until v.childCount).any { sliderAt(v.getChildAt(it), x, y) }
    }

    private fun track(e: MotionEvent, reset: Boolean = false) {
        if (reset) velocity?.clear()
        val v = velocity ?: VelocityTracker.obtain().also { velocity = it }
        v.addMovement(e)
    }

    override fun shouldDelayChildPressedState(): Boolean = true
}
