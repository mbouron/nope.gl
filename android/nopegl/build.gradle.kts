import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
    id("nopegl-nodes")
}

android {
    namespace = "org.nopeforge.nopegl"
    compileSdk = 34
    defaultConfig {
        minSdk = 29
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        consumerProguardFiles("consumer-rules.pro")
        externalNativeBuild {
            cmake {
                abiFilters("armeabi-v7a", "arm64-v8a", "x86_64")
                cFlags(
                    "-std=c17",
                    "-Wall",
                    "-Werror=pointer-arith",
                    "-Werror=vla",
                    "-Wformat=2",
                    "-Wignored-qualifiers",
                    "-Wimplicit-fallthrough",
                    "-Wlogical-op",
                    "-Wundef",
                    "-Wconversion",
                    "-Wno-sign-conversion",
                    "-fno-math-errno",
                )
                arguments("-DANDROID_STL=c++_shared")
            }
        }
    }
    packaging {
        jniLibs {
            keepDebugSymbols.addAll(
                listOf(
                    "*/arm64-v8a/*.so",
                    "*/armeabi-v7a/*.so",
                    "*/x86/*.so",
                    "*/x86_64/*.so",
                )
            )
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
    externalNativeBuild {
        cmake {
            path("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    sourceSets {
        getByName("test") {
            assets {
                srcDirs("src/androidTest/assets")
            }
        }
        getByName("main") {
            java {
                srcDir("src/shared/java")
            }
        }
    }
    // Add dependency for MockContentResolver
    useLibrary("android.test.mock")
}

dependencies {
    testImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test.ext:junit:1.1.5")
    androidTestImplementation("androidx.test.espresso:espresso-core:3.5.1")
    androidTestImplementation("androidx.test:runner:1.5.2")
    androidTestImplementation("androidx.test:rules:1.5.0")
}
