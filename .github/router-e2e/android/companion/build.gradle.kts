plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

kotlin {
    jvmToolchain(17)
}

android {
    namespace = "org.amnezia.routere2e.companion"
    compileSdk = 36

    defaultConfig {
        applicationId = "org.amnezia.routere2e.companion"
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
