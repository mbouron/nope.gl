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
    testImplementation(libs.kotlin.test)
    implementation(libs.kotlinpoet)
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.android.gradle)
}

tasks.test {
    useJUnitPlatform()
}
