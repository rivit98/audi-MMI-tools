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
        // APK instead of a same-version "Install". versionName is the build
        // time, shown in the phone's app info.
        val built = ZonedDateTime.now(ZoneOffset.UTC)
        val epoch = ZonedDateTime.of(2026, 1, 1, 0, 0, 0, 0, ZoneOffset.UTC)
        versionCode = Duration.between(epoch, built).toMinutes().toInt()
        versionName = built.format(DateTimeFormatter.ofPattern("yyyy.MM.dd-HHmm"))
    }

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

dependencies {
    testImplementation("junit:junit:4.13.2")
}
