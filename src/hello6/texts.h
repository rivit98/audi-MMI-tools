/*
 * texts.h - hello6's on-screen texts and its clip list, in one place so
 * the host preview (make hello6-preview) renders exactly what the car shows.
 */
#ifndef HELLO6_TEXTS_H
#define HELLO6_TEXTS_H

#define FOOTER_MAIN  "hello6 - results in hello6.log and hello6/ on the card"
#define FOOTER_VIDEO "hello6 - video test. Pull the SD card to stop."

#define INTRO_TITLE  "hello6: video + speed test (~5 min)"
#define INTRO_BODY \
	"Park the car. First 10 short test videos, each explained first. " \
	"Turn the main knob ONE click whenever you see a moving test " \
	"picture.\n\nThen test patterns flicker for about a minute: " \
	"nothing to do. They froze the MMI last time; if the picture " \
	"stops for 30 s, reboot it.\n\nLeave the card in until the " \
	"results screen has gone."

#define SYSINFO_TITLE "hello6: collecting system info"
#define SYSINFO_BODY  "Reading process, memory and GPU state. Nothing to do."
#define CPU_TITLE     "hello6: CPU benchmarks"
#define CPU_BODY      "memcpy, pixel conversion, LZ4. Nothing to do."
#define SUMMARY_TITLE "hello6 results (also in hello6.log)"

/* Video screen: "%s" is the clip's 'what'. */
#define VIDEO_TITLE_FMT "Video %d/%d: %s"
#define VIDEO_BODY_FMT \
	"%s\n\nFirst half: capture unit 0, second half: unit 1. Turn the " \
	"main knob ONE click whenever you see the moving test picture " \
	"(its name and a counting number)."

/* media/mkclips.sh makes these (same names, CLIP_S seconds each). */
#define CLIP_S 8

struct clip {
	const char *file, *title, *what;
	int grow;
};

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
