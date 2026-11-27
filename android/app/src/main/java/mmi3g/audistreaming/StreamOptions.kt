package mmi3g.audistreaming

import android.content.SharedPreferences
import android.view.View
import android.widget.AdapterView
import android.widget.CompoundButton
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.TextView

/**
 * The H.264 stream settings (res/layout/stream_options.xml): "Max fps",
 * "H.264 rate" and "Keep screen on", in [Streaming] for [AaService]. A
 * change is saved in [prefs].
 */
class StreamOptions(
    private val root: View,
    private val prefs: SharedPreferences,
    private val fpsText: (Int) -> String,
) {
    private val fpsLabel: TextView = root.findViewById(R.id.fps_label)
    private val fps: SeekBar = root.findViewById(R.id.fps)
    private val bitRate: Spinner = root.findViewById(R.id.bit_rate)
    private val keepScreenOn: CompoundButton = root.findViewById(R.id.keep_screen_on)
    private var showing = false                    // set from [Streaming]: no echo

    init {
        fps.max = Streaming.MAX_FPS - Streaming.MIN_FPS     // no setMin before API 26
        show()
        fps.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                fpsLabel.text = fpsText(value + Streaming.MIN_FPS)
                if (showing) return
                Streaming.maxFps = value + Streaming.MIN_FPS
                prefs.edit().putInt(PREF_FPS, Streaming.maxFps).apply()
            }
            override fun onStartTrackingTouch(bar: SeekBar) {}
            override fun onStopTrackingTouch(bar: SeekBar) {}
        })
        bitRate.onItemSelectedListener = selected { pos ->
            Streaming.bitRate = Streaming.BIT_RATES[pos]
            prefs.edit().putInt(PREF_BIT_RATE, Streaming.bitRate).apply()
        }
        keepScreenOn.setOnCheckedChangeListener { _, on ->
            if (!showing) {
                Streaming.keepScreenOn = on
                prefs.edit().putBoolean(PREF_KEEP_SCREEN_ON, on).apply()
            }
        }
    }

    /** The controls from [Streaming]. */
    fun show() {
        showing = true
        fps.progress = Streaming.maxFps - Streaming.MIN_FPS
        fpsLabel.text = fpsText(Streaming.maxFps)
        // animate = false: the selection is set now, not on the next layout
        bitRate.setSelection(Streaming.BIT_RATES.indexOf(Streaming.bitRate).coerceAtLeast(0), false)
        keepScreenOn.isChecked = Streaming.keepScreenOn
        showing = false
    }

    // A Spinner reports its selection again after a layout, also when set
    // from code: only a real change counts.
    private fun selected(apply: (Int) -> Unit) = object : AdapterView.OnItemSelectedListener {
        override fun onItemSelected(parent: AdapterView<*>, view: View?, pos: Int, id: Long) {
            if (Streaming.BIT_RATES[pos] == Streaming.bitRate) return
            apply(pos)
        }
        override fun onNothingSelected(parent: AdapterView<*>) {}
    }

    companion object {
        private const val PREF_FPS = "max_fps"
        private const val PREF_BIT_RATE = "bit_rate"
        private const val PREF_KEEP_SCREEN_ON = "keep_screen_on"

        /** [Streaming]'s settings from [prefs], once at the start. */
        fun load(prefs: SharedPreferences) {
            Streaming.maxFps = prefs.getInt(PREF_FPS, Streaming.DEFAULT_FPS)
                .coerceIn(Streaming.MIN_FPS, Streaming.MAX_FPS)
            Streaming.bitRate = prefs.getInt(PREF_BIT_RATE, Streaming.DEFAULT_BIT_RATE)
                .takeIf { it in Streaming.BIT_RATES } ?: Streaming.DEFAULT_BIT_RATE
            Streaming.keepScreenOn = prefs.getBoolean(PREF_KEEP_SCREEN_ON, false)
        }
    }
}
