import com.android.build.api.artifact.SingleArtifact
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    alias(libs.plugins.android.library)
    id("nopegl-nodes")
    `maven-publish`
}

val buildEnv = "${rootProject.projectDir.parent}/build-android"
val abis = listOf("armeabi-v7a", "arm64-v8a", "x86_64")

afterEvaluate {
    val repo = System.getenv("GITHUB_REPOSITORY") ?: "mbouron/nope.gl"
    val url = "https://github.com/$repo"
    val gitUrl = "$url.git"
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
                setUrl("https://maven.pkg.github.com/$repo")
                credentials {
                    val githubUsername: String? by project
                    val githubToken: String? by project
                    username = githubUsername ?: System.getenv("GITHUB_ACTOR")
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
                abiFilters(*abis.toTypedArray())
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
                arguments(
                    "-DANDROID_STL=c++_shared",
                    "-DNGL_ANDROID_ENV=${buildEnv}",
                )
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
    sourceSets {
        getByName("test") {
            assets {
                directories.add("src/androidTest/assets")
            }
        }
        getByName("main") {
            kotlin {
                directories.add("src/shared/java")
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

kotlin {
    compilerOptions {
        jvmTarget = JvmTarget.JVM_17
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

/*
 * Export the bundled FFmpeg and nope.media libraries through Prefab so other
 * native modules can use the same copies. Consumers enable `buildFeatures.prefab`,
 * use find_package(nopegl CONFIG) and link against nopegl::<module>.
 *
 * The libraries are built outside Gradle, so append the package to the final
 * AAR. Include relocatable pkg-config files for builds using Meson.
 */
data class PrefabModule(
    val headers: List<String>,
    val exports: List<String> = listOf(),
    val stl: String = "none",
)

val prefabModules = mapOf(
    "avutil" to PrefabModule(listOf("libavutil")),
    "swresample" to PrefabModule(listOf("libswresample"), listOf("avutil")),
    "swscale" to PrefabModule(listOf("libswscale"), listOf("avutil")),
    "avcodec" to PrefabModule(listOf("libavcodec"), listOf("avutil", "swresample")),
    // avformat uses BoringSSL, which needs the shared C++ runtime.
    "avformat" to PrefabModule(listOf("libavformat"), listOf("avcodec", "avutil"), stl = "c++_shared"),
    "avfilter" to PrefabModule(listOf("libavfilter"), listOf("avutil", "swscale", "swresample")),
    "avdevice" to PrefabModule(listOf("libavdevice"), listOf("avformat", "avcodec", "avutil"), stl = "c++_shared"),
    "nopemd" to PrefabModule(listOf("nopemd.h"), stl = "c++_shared"),
)

// Keep in sync with _ANDROID_VERSION in configure.py.
val nativeApiLevel = 29

val generatePrefab = tasks.register("generatePrefab") {
    val outputDir = layout.buildDirectory.dir("prefab")
    val ndkMajor = android.ndkVersion.substringBefore(".")
    val version = rootProject.file("../VERSION").readText().trim()
    inputs.property("version", version)
    inputs.property("ndkMajor", ndkMajor)
    for (abi in abis) {
        for ((name, module) in prefabModules) {
            inputs.file("$buildEnv/$abi/lib/lib$name.so")
            inputs.file("$buildEnv/$abi/lib/pkgconfig/lib$name.pc")
            for (header in module.headers)
                inputs.files("$buildEnv/$abi/include/$header")
        }
    }
    outputs.dir(outputDir)
    doLast {
        val root = outputDir.get().asFile
        root.deleteRecursively()
        root.mkdirs()
        File(root, "prefab.json").writeText(
            """{"schema_version": 2, "name": "nopegl", "version": "$version", "dependencies": []}"""
        )
        for ((name, module) in prefabModules) {
            val moduleDir = File(root, "modules/$name")
            moduleDir.mkdirs()
            val exports = module.exports.joinToString(", ") { "\":$it\"" }
            File(moduleDir, "module.json").writeText(
                """{"export_libraries": [$exports], "library_name": "lib$name"}"""
            )
            for (abi in abis) {
                val env = File(buildEnv, abi)
                val libDir = File(moduleDir, "libs/android.$abi")
                // Some headers depend on the ABI (e.g. libavutil/avconfig.h).
                for (header in module.headers) {
                    File(env, "include/$header").copyRecursively(File(libDir, "include/$header"))
                }
                File(env, "lib/lib$name.so").copyTo(File(libDir, "lib$name.so"))
                val pcLines = File(env, "lib/pkgconfig/lib$name.pc").readLines()
                val prefix = pcLines.first { it.startsWith("prefix=") }.substringAfter("=")
                val pc = pcLines.map { line ->
                    when {
                        line.startsWith("prefix=") -> "prefix=\${pcfiledir}"
                        line.startsWith("exec_prefix=") -> "exec_prefix=\${prefix}"
                        line.startsWith("libdir=") -> "libdir=\${prefix}"
                        line.startsWith("includedir=") -> "includedir=\${prefix}/include"
                        else -> line.replace(prefix, "\${prefix}")
                    }
                }
                File(libDir, "lib$name.pc").writeText(pc.joinToString("\n", postfix = "\n"))
                File(libDir, "abi.json").writeText(
                    """{"abi": "$abi", "api": $nativeApiLevel, "ndk": $ndkMajor, "stl": "${module.stl}", "static": false}"""
                )
            }
        }
    }
}

abstract class AddPrefabTask : DefaultTask() {
    @get:InputFile
    abstract val inputAar: RegularFileProperty

    @get:InputDirectory
    abstract val prefabDir: DirectoryProperty

    @get:OutputFile
    abstract val outputAar: RegularFileProperty

    @TaskAction
    fun run() {
        ZipFile(inputAar.get().asFile).use { aar ->
            ZipOutputStream(outputAar.get().asFile.outputStream()).use { out ->
                // Reuse the AAR timestamp to keep the archive reproducible.
                val time = aar.entries().nextElement().time
                for (entry in aar.entries()) {
                    out.putNextEntry(ZipEntry(entry.name).also { it.time = entry.time })
                    aar.getInputStream(entry).use { it.copyTo(out) }
                    out.closeEntry()
                }
                val root = prefabDir.get().asFile
                for (file in root.walkTopDown().filter { it.isFile }.sortedBy { it.path }) {
                    val name = "prefab/" + file.relativeTo(root).invariantSeparatorsPath
                    out.putNextEntry(ZipEntry(name).also { it.time = time })
                    file.inputStream().use { it.copyTo(out) }
                    out.closeEntry()
                }
            }
        }
    }
}

androidComponents {
    onVariants { variant ->
        val addPrefab = tasks.register<AddPrefabTask>("add${variant.name.replaceFirstChar { it.uppercase() }}Prefab") {
            dependsOn(generatePrefab)
            prefabDir.set(layout.buildDirectory.dir("prefab"))
        }
        variant.artifacts.use(addPrefab)
            .wiredWithFiles(AddPrefabTask::inputAar, AddPrefabTask::outputAar)
            .toTransform(SingleArtifact.AAR)
    }
}
