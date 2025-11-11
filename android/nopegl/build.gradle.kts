plugins {
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.android.library)
    id("nopegl-nodes")
    `maven-publish`
}

afterEvaluate {
    val url = "https://github.com/NopeForge/nope.gl"
    val gitUrl = "https://github.com/NopeForge/nope.gl.git"
    val nopeGlVersion = rootProject.file("../VERSION").readText().trim()
    val bindingsVersion = rootProject.file("VERSION").readText().trim()
    publishing {
        publications {
            register<MavenPublication>("release") {
                from(components["default"])
                groupId = "org.nopeforge.nopegl"
                artifactId = "android"
                val suffix = if (System.getenv("RELEASE_TAG_NAME") == null) "-SNAPSHOT" else ""
                this.version = "$nopeGlVersion-$bindingsVersion${suffix}"

                pom {
                    name = artifactId
                    description = "Android bindings for nope.gl"
                    this.url = url
                    scm {
                        connection = gitUrl
                        developerConnection = gitUrl
                        this.url = url
                    }
                }
            }
        }
        repositories {
            maven {
                name = "GitHubPackages"
                setUrl("https://maven.pkg.github.com/NopeForge/nope.gl")
                credentials {
                    val githubUsername: String? by project
                    val githubToken: String? by project
                    username = githubUsername ?: System.getenv("GITHUB_USERNAME")
                    password = githubToken ?: System.getenv("GITHUB_TOKEN")
                }
            }
        }
    }
}

android {
    namespace = "org.nopeforge.nopegl"
    compileSdk = 36
    ndkVersion = "28.1.13356709"
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
            consumerProguardFiles("consumer-rules.pro")
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

    publishing {
        multipleVariants {
            allVariants()
            withSourcesJar()
            withJavadocJar()
        }
    }
}

dependencies {
    implementation(platform(libs.coroutines.bom))
    implementation(libs.coroutines)
    implementation(libs.androidx.annotation.jvm)
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.test.runner)
    androidTestImplementation(libs.androidx.test.rules)
}
