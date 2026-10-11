plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

kotlin {
    jvmToolchain(17)
}

android {
    namespace = "org.amnezia.routere2e"
    compileSdk = 36

    defaultConfig {
        applicationId = "org.amnezia.routere2e"
        minSdk = 28
        targetSdk = 35
        versionCode = 1
        versionName = "1"
    }

    signingConfigs {
        create("e2e") {
            storeFile = file(System.getenv("ROUTER_E2E_KEYSTORE"))
            storePassword = "android"
            keyAlias = "androiddebugkey"
            keyPassword = "android"
        }
    }

    buildTypes {
        debug {
            signingConfig = signingConfigs.getByName("e2e")
        }
    }
}

dependencies {
    implementation("androidx.lifecycle:lifecycle-runtime:2.6.2")
    implementation("androidx.test:core-ktx:1.6.1")
    implementation("androidx.test:runner:1.6.2")
    implementation("androidx.test.ext:junit-ktx:1.2.1")
    implementation("junit:junit:4.13.2")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
}
