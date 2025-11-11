plugins {
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.android.application)
}

tasks.register("copyAssets") {
    val assetFiles = listOf(
        "cat.mp4",
        "mire-hevc.mp4"
    )

    val assetDir = "python/pynopegl-utils/pynopegl_utils/assets"
    val sourceDir = file("${project.rootDir}/../$assetDir")
    val assetsDir = file("${projectDir}/src/main/assets")

    doLast {
        assetsDir.mkdirs()

        assetFiles.forEach { filename ->
            val sourceFile = file("${sourceDir}/${filename}")
            if (!sourceFile.exists()) return@forEach

            val destFile = file("${assetsDir}/${filename}")
            if (!destFile.exists() || sourceFile.lastModified() > destFile.lastModified()) {
                sourceFile.copyTo(destFile, overwrite = true)
            }
        }
    }
}

tasks.named("preBuild").configure {
    dependsOn("copyAssets")
}

android {
    namespace = "org.nopeforge.nopegl"
    compileSdk = 36

    defaultConfig {
        minSdk = 29
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        vectorDrawables {
            useSupportLibrary = true
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        buildConfig = true
        compose = true
    }
    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.viewmodel.ktx)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.compose.activity)

    val composeBom = platform(libs.compose.bom)
    implementation(composeBom)
    androidTestImplementation(composeBom)

    implementation(libs.compose.material3)
    implementation(libs.compose.ui.tooling.preview)
    debugImplementation(libs.compose.ui.tooling)
    androidTestImplementation(libs.compose.ui.test.junit4)
    debugImplementation(libs.compose.ui.test.manifest)
    implementation(libs.compose.ui)
    implementation(libs.compose.ui.graphics)
    implementation(libs.compose.ui.tooling.preview)
    implementation(libs.compose.material3)
    implementation(libs.compose.constraintlayout)
    implementation(libs.timber)
    implementation(project(":nopegl"))
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.compose.ui.test.junit4)
    debugImplementation(libs.compose.ui.tooling)
    debugImplementation(libs.compose.ui.test.manifest)
}
