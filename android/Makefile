# usb_tx_test Android sender - everything runs in the mmi-android image
# (docker/Dockerfile: JDK 17, Android SDK 35, Gradle, platform-tools).
#
#   make            build out/usb_tx_test.apk
#   make test       Kotlin unit tests (protocol)
#   make install    adb install -r out/usb_tx_test.apk  (phone on this PC's USB,
#                   USB debugging on; adb runs in the container - stop a host
#                   adb server first: `adb kill-server`)
#   make shell      interactive shell in the build container
#   make clean      remove build outputs (keeps the Gradle cache and the key)
#   make distclean  also remove the Gradle cache
#
# Gradle's caches live in .cache/ (mounted), so rebuilds are offline-fast.
# The APK is signed with a fixed key in keystore/ (made once), so reinstalls
# keep working after `make clean`.

IMAGE := mmi-android
APK   := out/usb_tx_test.apk

RUN = mkdir -p .cache/home && docker run --rm -u $(shell id -u):$(shell id -g) \
      -e HOME=/src/.cache/home -e GRADLE_USER_HOME=/src/.cache/gradle \
      -v $(CURDIR):/src -w /src $(IMAGE)

# adb needs the USB device nodes (root in the container); its key is kept in
# a docker volume so the phone's "allow USB debugging" prompt comes once.
ADB = docker run --rm --privileged -v /dev/bus/usb:/dev/bus/usb \
      -v mmi-android-adb:/root/.android -v $(CURDIR)/out:/out:ro $(IMAGE)

SRC := $(shell find app/src -type f) app/build.gradle.kts build.gradle.kts \
       settings.gradle.kts gradle.properties

.PHONY: all image test install shell clean distclean
all: $(APK)

# Rebuilt when the Dockerfile changes (docker's own cache makes it cheap).
.cache/image.stamp: docker/Dockerfile
	docker build -t $(IMAGE) docker
	mkdir -p .cache && touch $@
image: .cache/image.stamp

keystore/debug.keystore: | .cache/image.stamp
	mkdir -p keystore
	$(RUN) keytool -genkeypair -keystore keystore/debug.keystore -storepass android \
	    -alias androiddebugkey -keypass android -keyalg RSA -keysize 2048 -validity 10000 \
	    -dname "CN=Android Debug,O=Android,C=US"

$(APK): $(SRC) keystore/debug.keystore .cache/image.stamp
	$(RUN) gradle --no-daemon -q assembleDebug
	mkdir -p out
	cp app/build/outputs/apk/debug/app-debug.apk $@
	@echo "built android/$@"

test: .cache/image.stamp
	$(RUN) gradle --no-daemon -q testDebugUnitTest
	@echo "android unit tests passed"

install: $(APK)
	$(ADB) sh -c 'adb start-server >/dev/null && adb wait-for-usb-device && \
	    adb install -r /out/usb_tx_test.apk; rc=$$?; adb kill-server; exit $$rc'

shell: .cache/image.stamp
	docker run --rm -it -u $(shell id -u):$(shell id -g) \
	    -e HOME=/src/.cache/home -e GRADLE_USER_HOME=/src/.cache/gradle \
	    -v $(CURDIR):/src -w /src $(IMAGE) bash

clean:
	rm -rf app/build build out .gradle

distclean: clean
	rm -rf .cache
