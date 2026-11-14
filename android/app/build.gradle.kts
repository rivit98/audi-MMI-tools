import java.time.Duration
import java.time.ZoneOffset
import java.time.ZonedDateTime
import java.time.format.DateTimeFormatter

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "mmi3g.audistreaming"
    compileSdk = 35
    buildToolsVersion = "35.0.0"

    defaultConfig {
        applicationId = "mmi3g.audistreaming"
        minSdk = 23
        targetSdk = 35
        // A new, higher versionCode on every build (minutes since 2026-01-01
        // UTC), so the phone's installer offers "Update" over the installed
        // APK instead of a same-version "Install". versionName is semantic
        // versioning with the build time (UTC) as build metadata, shown at the
        // bottom of the app and in the phone's app info. Bump semver by hand.
        val semver = "0.1.0"
        val built = ZonedDateTime.now(ZoneOffset.UTC)
        val epoch = ZonedDateTime.of(2026, 1, 1, 0, 0, 0, 0, ZoneOffset.UTC)
        versionCode = Duration.between(epoch, built).toMinutes().toInt()
        versionName = semver + "+" + built.format(DateTimeFormatter.ofPattern("yyyy.MM.dd-HHmm"))
        // the Android Auto session (src/android_auto) through the NDK; the
        // phone is arm64
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild { cmake { arguments += "-DANDROID_STL=c++_static" } }
    }

    ndkVersion = "27.2.12479018"
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    // aasdk's head unit certificate + key (the top Makefile fetches them),
    // as assets aa/headunit.crt and aa/headunit.key for the Android Auto mode
    sourceSets["main"].assets.srcDir(layout.buildDirectory.dir("generated/aa-assets"))

    // A fixed debug key in android/keystore/, so rebuilding (or `make clean`)
    // does not change the signature and `adb install -r` keeps working.
    signingConfigs {
        getByName("debug") {
            storeFile = rootProject.file("keystore/debug.keystore")
            storePassword = "android"
            keyAlias = "androiddebugkey"
            keyPassword = "android"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

val copyAaCert by tasks.registering(Copy::class) {
    from(rootProject.file("../third_party/aasdk-cert")) { include("headunit.crt", "headunit.key") }
    into(layout.buildDirectory.dir("generated/aa-assets/aa"))
}
tasks.named("preBuild") { dependsOn(copyAaCert) }

dependencies {
    testImplementation("junit:junit:4.13.2")
}
