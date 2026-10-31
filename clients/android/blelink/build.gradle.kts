plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.bluetoothserver.blelink"
    compileSdk = 35

    defaultConfig {
        minSdk = 23
        consumerProguardFiles("consumer-rules.pro")
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    testOptions {
        unitTests.isReturnDefaultValues = false
    }
}

dependencies {
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.7.3")

    testImplementation("junit:junit:4.13.2")
    // Real org.json for JVM unit tests (android.jar only has stubs); used to read docs/protocol-test-vectors.json.
    testImplementation("org.json:json:20231013")
}

// The test vectors live at the repo root (docs/protocol-test-vectors.json); tell the tests where.
tasks.withType<Test>().configureEach {
    systemProperty("blelink.vectors", rootProject.file("../../docs/protocol-test-vectors.json").absolutePath)
}
