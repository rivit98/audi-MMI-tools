# mmi-android - build image for the usb_tx_test Android sender (android/).
#
# JDK 17 + Android SDK command-line tools (platform 35, build-tools 35.0.0)
# + Gradle. Used by `make apk` / `make apk-test`; Gradle's dependency cache
# is NOT in the image but in build/android-home (mounted), so it survives
# image rebuilds and is reused across builds.
FROM eclipse-temurin:17.0.13_11-jdk-jammy

ARG CMDLINE_TOOLS=11076708
ARG GRADLE=8.11.1

RUN apt-get update \
 && apt-get install -y --no-install-recommends unzip wget \
 && rm -rf /var/lib/apt/lists/*

ENV ANDROID_HOME=/opt/android-sdk
ENV PATH=$PATH:/opt/android-sdk/platform-tools
RUN mkdir -p $ANDROID_HOME/cmdline-tools \
 && wget -q https://dl.google.com/android/repository/commandlinetools-linux-${CMDLINE_TOOLS}_latest.zip -O /tmp/ct.zip \
 && unzip -q /tmp/ct.zip -d $ANDROID_HOME/cmdline-tools \
 && mv $ANDROID_HOME/cmdline-tools/cmdline-tools $ANDROID_HOME/cmdline-tools/latest \
 && rm /tmp/ct.zip \
 && yes | $ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager --licenses > /dev/null \
 && $ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager --install \
        "platforms;android-35" "build-tools;35.0.0" "platform-tools" > /dev/null \
 && chmod -R a+rX $ANDROID_HOME

RUN wget -q https://services.gradle.org/distributions/gradle-${GRADLE}-bin.zip -O /tmp/g.zip \
 && unzip -q /tmp/g.zip -d /opt \
 && ln -s /opt/gradle-${GRADLE}/bin/gradle /usr/local/bin/gradle \
 && rm /tmp/g.zip

WORKDIR /src/android
