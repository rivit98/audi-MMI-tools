package mmi3g.audistreaming

import android.content.SharedPreferences
import android.view.View
import android.widget.AdapterView
import android.widget.CompoundButton
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.TextView

/**
 * One copy of the H.264 stream settings (res/layout/stream_options.xml):
 * "Max fps", "H.264 rate" and "Keep screen on". Both pages
 * have one, and both modes use the same values ([Streaming]): the screen
 * mirroring ([CaptureService]) and Android Auto ([AaService]). A change in
 * one copy is saved in [prefs], and [onChange] lets the owner [show] it in
 * the other.
 */
class StreamOptions(
    private val root: View,
    private val prefs: SharedPreferences,
    private val fpsText: (Int) -> String,
    private val onChange: () -> Unit,
) {
    private val fpsLabel: TextView = root.findViewById(R.id.fps_label)
    private val fps: SeekBar = root.findViewById(R.id.fps)
    private val bitRate: Spinner = root.findViewById(R.id.bit_rate)
    private val keepScreenOn: CompoundButton = root.findViewById(R.id.keep_screen_on)
    private var showing = false                    // set from [Streaming]: no echo

    init {
        fps.max = CaptureService.MAX_FPS - CaptureService.MIN_FPS     // no setMin before API 26
        show()
        fps.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                fpsLabel.text = fpsText(value + CaptureService.MIN_FPS)
                if (showing) return
                Streaming.maxFps = value + CaptureService.MIN_FPS
                prefs.edit().putInt(PREF_FPS, Streaming.maxFps).apply()
                onChange()
            }
            override fun onStartTrackingTouch(bar: SeekBar) {}
            override fun onStopTrackingTouch(bar: SeekBar) {}
        })
        bitRate.onItemSelectedListener = selected { pos ->
            Streaming.bitRate = CaptureService.BIT_RATES[pos]
            prefs.edit().putInt(PREF_BIT_RATE, Streaming.bitRate).apply()
        }
        keepScreenOn.setOnCheckedChangeListener { _, on ->
            if (!showing) {
                Streaming.keepScreenOn = on
                prefs.edit().putBoolean(PREF_KEEP_SCREEN_ON, on).apply()
                onChange()
            }
        }
    }

    /** The controls from [Streaming]. */
    fun show() {
        showing = true
        fps.progress = Streaming.maxFps - CaptureService.MIN_FPS
        fpsLabel.text = fpsText(Streaming.maxFps)
        // animate = false: the selection is set now, not on the next layout
        bitRate.setSelection(CaptureService.BIT_RATES.indexOf(Streaming.bitRate).coerceAtLeast(0), false)
        keepScreenOn.isChecked = Streaming.keepScreenOn
        showing = false
    }

    // A Spinner reports its selection again after a layout, also when set
    // from code: only a real change counts.
    private fun selected(apply: (Int) -> Unit) = object : AdapterView.OnItemSelectedListener {
        override fun onItemSelected(parent: AdapterView<*>, view: View?, pos: Int, id: Long) {
            if (CaptureService.BIT_RATES[pos] == Streaming.bitRate) return
            apply(pos)
            onChange()
        }
        override fun onNothingSelected(parent: AdapterView<*>) {}
    }

    companion object {
        private const val PREF_FPS = "max_fps"
        private const val PREF_BIT_RATE = "bit_rate"
        private const val PREF_KEEP_SCREEN_ON = "keep_screen_on"

        /** [Streaming]'s settings from [prefs], once at the start. */
        fun load(prefs: SharedPreferences) {
            Streaming.maxFps = prefs.getInt(PREF_FPS, CaptureService.DEFAULT_FPS)
                .coerceIn(CaptureService.MIN_FPS, CaptureService.MAX_FPS)
            Streaming.bitRate = prefs.getInt(PREF_BIT_RATE, CaptureService.DEFAULT_BIT_RATE)
                .takeIf { it in CaptureService.BIT_RATES } ?: CaptureService.DEFAULT_BIT_RATE
            Streaming.keepScreenOn = prefs.getBoolean(PREF_KEEP_SCREEN_ON, false)
        }
    }
}
