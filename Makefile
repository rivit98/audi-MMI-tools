# rivit_app - native apps for my MMI 3G+ (see README.md)
#
#   make sdk             build the mmi-sdk Docker image (toolchain + QNX sysroot)
#   make                 cross-compile all apps into build/
#   make check APP=x     show what build/x looks like (ELF header, deps)
#   make sd              assemble build/sd/: on SD insert run.sh runs every test
#                        app in turn, with status screens (showScreen) in between,
#                        rendered on the car by pngondemand
#   make sd SD_APPS="hello hello3"
#                        the same with only those apps (in that order)
#   make test            host-side unit tests (tests/*.c) in the SDK container
#   make apk             build the Android sender: android/out/usb_tx_test.apk
#                        (= make -C android; see android/Makefile)
#   make apk-test        its Kotlin unit tests
#   make apk-install     adb install it (adb in the container, phone on USB)
#   make aoa-host        desk test: this PC plays the MMI for the phone app
#   make ui-preview      render usb_tx_test's screen on the host (build/ui_*.bmp)
#   make hello6-preview  render hello6's instruction screens (build/hello6_*.bmp)
#   make screens-preview render run.sh's status screens (build/screens/*.png)
#   make screens-sim     the screens of one simulated run (build/screens-sim/)
#   make addr APP=x ADDR="0x8043270 ..."
#                        crash address(es) in build/x -> function, file:line
#   make clips           hello6's test videos (build/clips/, ffmpeg in the
#                        mmi-media image; `make sd SD_APPS=hello6` does it too)
#   make shell           interactive shell in the SDK container

REPO   := $(abspath $(CURDIR)/../..)
OQ     := $(REPO)/dev/openqnx/trunk
FW     := $(REPO)/dev/HN+R_EU_AU_P1001/extracted/MU9411/ifs-root/51
FWEFS  := $(REPO)/dev/HN+R_EU_AU_P1001/extracted/MU9411/efs-system/51
IMAGE  := mmi-sdk
APP    ?= hello2

# Device libraries we can link against (stubbed in the image).
FWLIBS := $(FW)/proc/boot/libc.so.2 \
          $(addprefix $(FW)/mnt/ifs-root/usr/lib/,libm.so.2 libgf.so.1 libEGL.so.6 libGLES_CM.so.6 libasound.so.2) \
          $(FWEFS)/lib/libusbdi.so.2

RUN := docker run --rm -u $(shell id -u):$(shell id -g) \
       -v $(CURDIR):/src -v $(REPO)/builder:/toolkit-builder:ro -v $(REPO)/core:/toolkit-core:ro \
       -w /src $(IMAGE)

# -O2 needs GCC 16: Debian's sh4 GCC 12.2 miscompiled `a == 0 && b == 0` at
# -O1 and up (docs/gcc-sh4-treg-bug.md). build/.canary stops the build if it
# comes back.
# common/qnx/: a <math.h> for stb_image_write (the sysroot has none), car build only
CFLAGS := -O2 -g -Wall -Isrc/common -Isrc/common/qnx
COMMON := $(wildcard src/common/*.c src/common/*.h)

# Apps: build/<app> from src/<app>/*.c (+ src/common for graphics apps).
# APPS is everything `make` builds, and all of it goes on the card; TESTS
# are what run.sh executes by default, in this order (settings:
# sd/apps.conf).
#
# hello and lmprobe are built and shipped but no longer run: both have
# answered their question on the car. hello proved the toolchain produces
# binaries the MMI runs; lmprobe proved the layer-manager / VFB sequence,
# and its d-shot is byte-identical between runs (docs/platform-notes.md).
# Run either by hand from the card, or with `make sd SD_APPS="lmprobe ..."`.
# hello2 and hello3 joined them after the 2026-10-11 run: both passed, and
# what they measured is written up in docs/gpu-results.md.
#
# hello4's stress scene froze the whole unit on 2026-10-11 and is now run
# to find out why (sd/apps.conf). A freeze ends the run, so hello4 goes
# after hello5 and hello6: only usb_tx_test, which has to be last, is lost
# if it happens again.
#
# hello6 needs the driver watching for about five minutes and puts ~16 MB of
# test videos on the card. usb_tx_test runs until the SD card is pulled, so
# it is always last: pulling the card ends it and the whole run (no final
# "done" screen; everything before it is already on the card).
APPS    := hello lmprobe hello2 hello3 hello4 hello5 hello6 usb_tx_test
# Tools run.sh uses, also built for the car and put in bin/ on the card.
TOOLS   := pngondemand
TESTS   := hello5 hello6 hello4 usb_tx_test
SD_APPS ?= $(TESTS)
# Seconds each status screen is shown: before an app, after it, at the end.
PRE_S   := 5
POST_S  := 8
FINAL_S := 15

.PHONY: all sdk check sd addr shell clean test apk apk-test apk-install aoa-host ui-preview clips \
        hello6-preview screens-preview screens-sim
all: $(addprefix build/,$(APPS) $(TOOLS))

build/hello:  $(wildcard src/hello/*.c)
# lmprobe: layer manager only (no GL).
build/lmprobe: $(wildcard src/lmprobe/*.c src/lmprobe/*.h) \
               src/common/lmgr.c src/common/log.c src/common/quit.c \
               src/common/lmgr.h src/common/log.h src/common/quit.h
LIBS_lmprobe := -lgf
build/hello2: $(wildcard src/hello2/*.c) $(COMMON)
LIBS_hello2 := -lGLES_CM -lgf
build/hello3: $(wildcard src/hello3/*.c src/hello3/*.h) $(COMMON)
LIBS_hello3 := -lGLES_CM -lgf -lm
# hello5 has no graphics: it needs only the log + quit helpers, not gfx/hud.
build/hello5: $(wildcard src/hello5/*.c) src/common/log.c src/common/quit.c \
              src/common/log.h src/common/quit.h
LIBS_hello5 := -lasound
build/hello4: $(wildcard src/hello4/*.c src/hello4/*.h) $(COMMON)
LIBS_hello4 := -lGLES_CM -lgf -lm
# usb_tx_test: USB (libusbdi) + CPU-drawn layer-manager screen, no GL.
# Its fonts are generated from sdk/fonts by sdk/mkfont.py into build/gen/.
USBTX := $(wildcard src/usb_tx_test/*.c src/usb_tx_test/*.h)
build/usb_tx_test: $(USBTX) build/gen/font_big.h build/gen/font_small.h \
                   src/common/lmgr.c src/common/log.c src/common/quit.c \
                   src/common/lmgr.h src/common/log.h src/common/quit.h
LIBS_usb_tx_test   := -lusbdi -lgf
CFLAGS_usb_tx_test := -Ibuild/gen -Isrc/usb_tx_test

# hello6: mirroring assumptions (docs/phone-mirroring.md). GL benchmarks
# (common/gfx) + usb_tx_test's CPU text screen for its instruction screens.
build/hello6: $(wildcard src/hello6/*.c src/hello6/*.h) $(COMMON) \
              src/usb_tx_test/text.c src/usb_tx_test/text.h \
              src/usb_tx_test/screen.c src/usb_tx_test/screen.h \
              build/gen/font_big.h build/gen/font_small.h
LIBS_hello6   := -lGLES_CM -lgf
CFLAGS_hello6 := -Ibuild/gen -Isrc/usb_tx_test -Isrc/hello6

# pngondemand: layout text -> PNG (src/pngondemand/README.md), for run.sh's
# status screens. usb_tx_test's text drawing and fonts + common/img.c (the
# vendored stb_image_write.h), no device libraries.
PNGOD := $(wildcard src/pngondemand/*.c src/pngondemand/*.h) \
         src/usb_tx_test/text.c src/usb_tx_test/text.h \
         src/common/img.c src/common/img.h src/common/stb_image_write.h
build/pngondemand: $(PNGOD) build/gen/font_big.h build/gen/font_small.h
CFLAGS_pngondemand := -Ibuild/gen -Isrc/usb_tx_test

build/gen/font_big.h: sdk/mkfont.py sdk/fonts/Uni2-TerminusBold32x16.psf.gz
	mkdir -p build/gen
	$(RUN) python3 -I sdk/mkfont.py sdk/fonts/Uni2-TerminusBold32x16.psf.gz font_big $@
build/gen/font_small.h: sdk/mkfont.py sdk/fonts/Uni2-TerminusBold16.psf.gz
	mkdir -p build/gen
	$(RUN) python3 -I sdk/mkfont.py sdk/fonts/Uni2-TerminusBold16.psf.gz font_small $@

sdk:
	rm -rf build/fwlibs && mkdir -p build/fwlibs && cp $(FWLIBS) build/fwlibs/
	docker build -t $(IMAGE) \
	    --build-context qnx-libc-public=$(OQ)/lib/c/public \
	    --build-context qnx-sys-public=$(OQ)/services/system/public \
	    --build-context qnx-crt-sh=$(OQ)/lib/c/startup/sh \
	    --build-context fwlibs=build/fwlibs \
	    .

$(addprefix build/,$(APPS) $(TOOLS)): build/.canary

# The compiler canary: sdk/canary.c's `a == 0 && *p == 0` needs two
# compares (tst / cmp). With the miscompile only one is left.
build/.canary: sdk/canary.c Makefile
	mkdir -p build
	$(RUN) shle-nto-gcc $(CFLAGS) -S -o build/canary.s sdk/canary.c
	@n=$$(awk '/^canary_and:/,/\.size/' build/canary.s | grep -cE '^[[:space:]]+(tst|cmp/)'); \
	if [ "$$n" -lt 2 ]; then \
	    echo "COMPILER CANARY FAILED: canary_and has $$n compare(s), needs 2."; \
	    echo "CFLAGS ($(CFLAGS)) hit the sh4 GCC miscompile, see docs/gcc-sh4-treg-bug.md"; \
	    exit 1; \
	fi; echo "compiler canary ok ($$n compares)"
	touch $@

build/%:
	mkdir -p build
	$(RUN) shle-nto-gcc $(CFLAGS) $(CFLAGS_$*) -o $@ $(filter %.c,$^) $(LIBS_$*)

check: build/$(APP)
	$(RUN) sh -c 'file build/$(APP); sh4-linux-gnu-readelf -h -l -d build/$(APP) | grep -E "Type:|Entry|interpreter|LOAD|NEEDED|HASH|FLAGS|Flags"'

# hello6's extras. Its test videos go on the card only when it runs
# (~16 MB). The unit's own efs tools it uses (io-media test client, layer
# manager / GPU memory / CPU load tools) always go along, in bin/: hello6
# prefers the unit's copies and falls back to these. They are the unit's
# files, inflated from efs-system (tools/qnx_inflator.c) into $(FWEFS)/bin.
FWTOOLS   := $(wildcard $(addprefix $(FWEFS)/bin/,testapp-file testapp-cmdline mmecli \
             ShowVideoMemory memusage hbhogs))
HELLO6_SD := $(if $(filter hello6,$(SD_APPS)),build/clips/.done)

# Every app we build goes on the card (so anything can be run by hand from
# /tmp); only SD_APPS are executed by run.sh and get status screens.
sd: $(addprefix build/,$(APPS) $(TOOLS)) $(HELLO6_SD)
	rm -rf build/sd && mkdir -p build/sd/bin build/sd/var
	cp $(addprefix build/,$(APPS) $(TOOLS)) $(FWTOOLS) $(REPO)/core/bin/showScreen build/sd/bin/
	cp sd/apps.conf sd/screens.sh sd/README.md build/sd/
	@if [ -n "$(HELLO6_SD)" ]; then \
	    mkdir -p build/sd/clips && cp build/clips/*.* build/sd/clips/; \
	    echo "bundled build/clips -> card:/clips (hello6)"; \
	fi
	@if echo " $(APPS) " | grep -q " hello5 "; then \
	    cp src/hello5/beep.wav build/sd/beep.wav; \
	    echo "bundled src/hello5/beep.wav -> card:/beep.wav"; \
	fi
	sed -e 's/@APPS@/$(SD_APPS)/' -e 's/@PRE_S@/$(PRE_S)/' \
	    -e 's/@POST_S@/$(POST_S)/' -e 's/@FINAL_S@/$(FINAL_S)/' sd/run.sh > build/sd/run.sh
	$(RUN) python3 -I /toolkit-builder/encoder.py encode \
	    /toolkit-core/copie_scr_plain.sh build/sd/copie_scr.sh
	@echo "SD card layout ready in build/sd/ - copy its contents to the card root"
	@echo "  on the card: $(APPS)"
	@echo "  run by run.sh: $(SD_APPS)"

# hello6's test videos: media/mkclips.sh in the mmi-media image (ffmpeg).
MEDIA_IMAGE := mmi-media
build/.media-image: media/Dockerfile
	docker build -t $(MEDIA_IMAGE) media
	mkdir -p build && touch $@
build/clips/.done: media/mkclips.sh build/.media-image
	rm -rf build/clips && mkdir -p build/clips
	docker run --rm -u $(shell id -u):$(shell id -g) -v $(CURDIR):/src -w /src \
	    $(MEDIA_IMAGE) bash media/mkclips.sh build/clips
	touch $@
clips: build/clips/.done

# Our binaries are ET_EXEC at a fixed base, so an ip/BT address from the
# dumper's slog line or core file maps straight to the unstripped build/x.
addr: build/$(APP)
	$(RUN) sh4-linux-gnu-addr2line -f -i -p -e build/$(APP) $(ADDR)

# Host-side unit tests of the pure parts (protocol, UTF-8/wrap/draw), built
# with the SDK image's native gcc + ASan/UBSan.
TESTBIN := build/test_proto build/test_text build/test_lz4 build/test_png build/test_layout
test: $(TESTBIN)
	@for t in $(TESTBIN); do $(RUN) ./$$t || exit 1; done
	$(RUN) python3 -I tests/check_png.py build/test_png.d
build/test_proto: tests/test_proto.c tests/check.h src/usb_tx_test/proto.c src/usb_tx_test/proto.h
build/test_text:  tests/test_text.c tests/check.h src/usb_tx_test/text.c src/usb_tx_test/text.h
# lz4mini against the reference liblz4 (liblz4-dev in the SDK image)
build/test_lz4:   tests/test_lz4.c tests/check.h src/hello6/lz4mini.c src/hello6/lz4mini.h
TESTLIBS_test_lz4 := -llz4
# the PNG output (common/img.c over stb_image_write): test_png writes PNGs + their raw pixels
# into build/test_png.d/, tests/check_png.py decodes them with Python's zlib.
build/test_png:    tests/test_png.c tests/check.h src/common/img.c src/common/img.h \
                   src/common/stb_image_write.h src/usb_tx_test/text.c
build/test_layout: tests/test_layout.c tests/check.h src/pngondemand/layout.c \
                   src/pngondemand/layout.h src/usb_tx_test/text.c \
                   build/gen/font_big.h build/gen/font_small.h
$(TESTBIN):
	mkdir -p build
	$(RUN) gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Itests -Isrc/usb_tx_test \
	    -Isrc/hello6 -Isrc/pngondemand -Isrc/common -Ibuild/gen -o $@ $(filter %.c,$^) $(TESTLIBS_$(@F))

# The usb_tx_test screen rendered on the host for every link state:
# build/ui_*.bmp (layout check without the car).
ui-preview: build/gen/font_big.h build/gen/font_small.h
	$(RUN) gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc/common -Isrc/usb_tx_test \
	    -Ibuild/gen -o build/ui_preview tests/ui_preview.c src/usb_tx_test/ui.c \
	    src/usb_tx_test/text.c src/usb_tx_test/model.c src/common/log.c src/common/img.c -lpthread
	$(RUN) ./build/ui_preview

# hello6's instruction screens rendered on the host: build/hello6_*.bmp.
# Fails if any screen's text does not fit.
hello6-preview: build/gen/font_big.h build/gen/font_small.h
	$(RUN) gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc/common -Isrc/usb_tx_test \
	    -Isrc/hello6 -Ibuild/gen -o build/hello6_preview tests/hello6_preview.c \
	    src/hello6/label.c src/usb_tx_test/text.c src/common/img.c
	$(RUN) ./build/hello6_preview

# run.sh's status screens rendered on the host, from the real sd/screens.sh
# and apps.conf by a host build of pngondemand: build/screens/*.png. Fails
# if any screen's text does not fit.
screens-preview: build/gen/font_big.h build/gen/font_small.h
	mkdir -p build/host
	$(RUN) gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc/usb_tx_test -Ibuild/gen \
	    -Isrc/common -o build/host/pngondemand $(filter %.c,$(PNGOD))
	$(RUN) bash tests/screens_preview.sh build/host/pngondemand build/screens \
	    "$(SD_APPS)" $(PRE_S) $(POST_S) $(FINAL_S)

# One simulated run of the hello apps: build/screens-sim/NN_*.png in the
# order run.sh shows them. SIM: app:verdict:exit code:run time:FAIL lines.
SIM ?= hello2:OK:0:24:0 hello3:OK:0:37:0 hello4:CRASH:139:23:0 \
       hello5:WARN:0:31:2 hello6:TIMEOUT:143:735:0
screens-sim: screens-preview
	$(RUN) bash tests/screens_simulate.sh build/host/pngondemand build/screens-sim \
	    $(PRE_S) $(POST_S) $(FINAL_S) $(SIM)

# Android sender: its own Makefile (android/Makefile), everything in Docker.
apk:
	$(MAKE) -C android
apk-test:
	$(MAKE) -C android test
apk-install:
	$(MAKE) -C android install

# Desk test of the phone app without the car: tools/aoa_host.py plays the
# MMI (AOA handshake + protocol) over the PC's USB. Stop adb first.
aoa-host:
	docker build -q -t mmi-aoa-host -f tools/Dockerfile.aoa-host tools
	docker run --rm -it --privileged -v /dev/bus/usb:/dev/bus/usb mmi-aoa-host

shell:
	docker run --rm -it -u $(shell id -u):$(shell id -g) -v $(CURDIR):/src -w /src $(IMAGE) bash

clean:
	rm -rf build
