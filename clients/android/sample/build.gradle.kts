plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.bluetoothserver.blelink.sample"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.bluetoothserver.blelink.sample"
        minSdk = 23
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"
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
    implementation(project(":blelink"))
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.7.3")
}
