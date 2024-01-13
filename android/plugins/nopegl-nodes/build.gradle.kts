plugins {
    `kotlin-dsl`
    kotlin("jvm").version(KotlinVersion.CURRENT.toString())
    id("org.jetbrains.kotlin.plugin.serialization").version(KotlinVersion.CURRENT.toString())
}

kotlin {
    jvmToolchain(17)
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(17))
    }
}

sourceSets {
    main {
        java {
            srcDir(rootDir.parentFile.resolve("nopegl/src/shared/java"))
        }
    }
}

gradlePlugin {
    plugins {
        register("nopegl-nodes") {
            id = "nopegl-nodes"
            implementationClass = "org.nopeforge.nopegl.GenerateNodesPlugin"
        }
    }
}

dependencies {
    testImplementation("org.jetbrains.kotlin:kotlin-test")
    implementation("com.squareup:kotlinpoet:1.16.0")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.6.3")
    implementation("com.android.tools.build:gradle:8.2.0")
}

tasks.test {
    useJUnitPlatform()
}