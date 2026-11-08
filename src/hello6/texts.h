/*
 * texts.h - hello6's on-screen texts and its clip list, in one place.
 */
#ifndef HELLO6_TEXTS_H
#define HELLO6_TEXTS_H

#define FOOTER_MAIN  "hello6 - results in hello6.log and hello6/ on the card"
#define FOOTER_VIDEO "hello6 - video test. Pull the SD card to stop."

#define INTRO_TITLE  "hello6: video test (~6 min)"
#define INTRO_BODY \
	"Park the car. Ten test videos, each shown two ways one after " \
	"the other (about 7 s each). Turn the main knob ONE click " \
	"whenever you see the moving test picture.\n\nLeave the card " \
	"in until the results screen has gone."

#define SYSINFO_TITLE "hello6: collecting system info"
#define SYSINFO_BODY  "Reading process, memory and GPU state. Nothing to do."
#define CPU_TITLE     "hello6: CPU benchmarks"
#define CPU_BODY      "memcpy, pixel conversion, LZ4. Nothing to do."
#define SUMMARY_TITLE "hello6 results (also in hello6.log)"

/* Video screen: "%s" is the clip's 'what'. */
#define VIDEO_TITLE_FMT "Video %d/%d: %s"
#define VIDEO_BODY_FMT \
	"%s\n\nFirst capture unit 1 (partial decode), then unit 0 (DSP " \
	"video port), about 7 s each. Turn the main knob ONE click " \
	"whenever you see the moving test picture (its name and a " \
	"counting number)."

/* media/mkclips.sh makes these (same names, CLIP_S seconds each: long
 * enough for both capture-unit attempts, which take ~20 s per clip).
 * The picture is always seen within 4 s. */
#define CLIP_S  22

struct clip {
	const char *file, *title, *what;
	int grow;
};

/* Clips 1-10 give the same result every run. The RE (docs/phone-mirroring.md
 * 7.8) explains 4 (io-media refuses
 * width > 720) and 7/9 (mp4_parser ignores moof). The streaming tests that
 * followed (constant-sample-size MP4, a live stream) are hello9's. */
static const struct clip clips[] = {
	{ "c1_mpeg2_576.mpg", "MPEG-2 720x576 (DVD-like)",
	  "Control: the DVD player's format. Not seen here = the screen "
	  "layer is wrong, not the H.264 decoder.", 0 },
	{ "c2_mpeg4_480.avi", "MPEG-4 Part 2 640x480 AVI",
	  "Control 2: DivX-style video, another DSP decoder.", 0 },
	{ "c3_h264_480.mp4", "H.264 Baseline 720x480 MP4",
	  "H.264 at DVD width: inside every known limit.", 0 },
	{ "c4_h264_800.mp4", "H.264 Baseline 800x480 MP4",
	  "Full screen width: does the 'width < 720' limit bite?", 0 },
	{ "c5_h264_336.mp4", "H.264 Baseline 720x336 MP4",
	  "The letterbox size for a landscape phone.", 0 },
	{ "c6_h264_336_na.mp4", "H.264 720x336 MP4, no audio",
	  "Same video without an audio track.", 0 },
	{ "c7_h264_336_frag.mp4", "H.264 720x336 fragmented MP4",
	  "The streamable MP4 layout (moof fragments).", 0 },
	{ "c8_h264_336.mpg", "H.264 720x336 in MPEG-PS",
	  "The other streamable container.", 0 },
	{ "c7_h264_336_frag.mp4", "GROWING fragmented MP4",
	  "Written to the card while it plays: live-stream stand-in.", 1 },
	{ "c8_h264_336.mpg", "GROWING MPEG-PS",
	  "Written to the card while it plays: live-stream stand-in.", 1 },
};
#define NCLIPS ((int)(sizeof(clips) / sizeof(clips[0])))

#endif
