# rivit_app - native apps for my MMI 3G+ (see README.md)
#
#   make sdk             build the mmi-sdk Docker image (toolchain + QNX sysroot)
#   make                 cross-compile all apps into build/
#   make check APP=x     show what build/x looks like (ELF header, deps)
#   make sd              assemble build/sd/: on SD insert run.sh runs every test
#                        app in turn, with status screens (showScreen) in between
#   make sd SD_APPS="hello hello3"
#                        the same with only those apps (in that order)
#   make addr APP=x ADDR="0x8043270 ..."
#                        crash address(es) in build/x -> function, file:line
#   make shell           interactive shell in the SDK container

REPO   := $(abspath $(CURDIR)/../..)
OQ     := $(REPO)/dev/openqnx/trunk
FW     := $(REPO)/dev/HN+R_EU_AU_P1001/extracted/MU9411/ifs-root/51
IMAGE  := mmi-sdk
APP    ?= hello2

# Device libraries we can link against (stubbed in the image).
FWLIBS := $(FW)/proc/boot/libc.so.2 \
          $(addprefix $(FW)/mnt/ifs-root/usr/lib/,libm.so.2 libgf.so.1 libEGL.so.6 libGLES_CM.so.6 libasound.so.2)

RUN := docker run --rm -u $(shell id -u):$(shell id -g) \
       -v $(CURDIR):/src -v $(REPO)/builder:/toolkit-builder:ro -v $(REPO)/core:/toolkit-core:ro \
       -w /src $(IMAGE)

CFLAGS := -O2 -g -Wall -Isrc/common
COMMON := $(wildcard src/common/*.c src/common/*.h)

# Apps: build/<app> from src/<app>/*.c (+ src/common for graphics apps).
# TESTS are what run.sh runs, in this order (settings: sd/apps.conf).
TESTS   := hello hello2 hello3 hello4 hello5
APPS    := $(TESTS)
SD_APPS ?= $(TESTS)
# Seconds each status screen is shown: before an app, after it, at the end.
PRE_S   := 5
POST_S  := 8
FINAL_S := 15

.PHONY: all sdk check sd addr shell clean
all: $(addprefix build/,$(APPS))

build/hello:  $(wildcard src/hello/*.c)
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

sdk:
	rm -rf build/fwlibs && mkdir -p build/fwlibs && cp $(FWLIBS) build/fwlibs/
	docker build -t $(IMAGE) \
	    --build-context qnx-libc-public=$(OQ)/lib/c/public \
	    --build-context qnx-sys-public=$(OQ)/services/system/public \
	    --build-context qnx-crt-sh=$(OQ)/lib/c/startup/sh \
	    --build-context fwlibs=build/fwlibs \
	    .

build/%:
	mkdir -p build
	$(RUN) shle-nto-gcc $(CFLAGS) -o $@ $(filter %.c,$^) $(LIBS_$*)

check: build/$(APP)
	$(RUN) sh -c 'file build/$(APP); sh4-linux-gnu-readelf -h -l -d build/$(APP) | grep -E "Type:|Entry|interpreter|LOAD|NEEDED|HASH|FLAGS|Flags"'

sd: $(addprefix build/,$(SD_APPS))
	rm -rf build/sd && mkdir -p build/sd/bin build/sd/var
	cp $^ $(REPO)/core/bin/showScreen build/sd/bin/
	cp sd/apps.conf sd/README.md build/sd/
	@if echo " $(SD_APPS) " | grep -q " hello5 "; then \
	    cp src/hello5/beep.wav build/sd/beep.wav; \
	    echo "bundled src/hello5/beep.wav -> card:/beep.wav"; \
	fi
	sed -e 's/@APPS@/$(SD_APPS)/' -e 's/@PRE_S@/$(PRE_S)/' \
	    -e 's/@POST_S@/$(POST_S)/' -e 's/@FINAL_S@/$(FINAL_S)/' sd/run.sh > build/sd/run.sh
	$(RUN) python3 -I sdk/mkscreens.py build/sd/screens sd/apps.conf \
	    $(PRE_S) $(POST_S) $(FINAL_S) $(SD_APPS)
	$(RUN) python3 -I /toolkit-builder/encoder.py encode \
	    /toolkit-core/copie_scr_plain.sh build/sd/copie_scr.sh
	@echo "SD card layout for $(SD_APPS) ready in build/sd/ - copy its contents to the card root"

# Our binaries are ET_EXEC at a fixed base, so an ip/BT address from the
# dumper's slog line or core file maps straight to the unstripped build/x.
addr: build/$(APP)
	$(RUN) sh4-linux-gnu-addr2line -f -i -p -e build/$(APP) $(ADDR)

shell:
	docker run --rm -it -u $(shell id -u):$(shell id -g) -v $(CURDIR):/src -w /src $(IMAGE) bash

clean:
	rm -rf build
