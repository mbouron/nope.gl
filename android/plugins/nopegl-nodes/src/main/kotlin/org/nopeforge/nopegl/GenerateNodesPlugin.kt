package org.nopeforge.nopegl

import com.android.build.api.variant.LibraryAndroidComponentsExtension
import org.gradle.api.Plugin
import org.gradle.api.Project
import org.nopeforge.nopegl.kotlin.toCamelCase
import org.nopeforge.nopegl.task.GenerateNodesTask
import java.io.File

class GenerateNodesPlugin : Plugin<Project> {
    override fun apply(target: Project) {
        with(target) {
            val nglAndroidEnvironment = requireNotNull(System.getenv("NGL_ANDROID_ENV")) {
                "NGL_ANDROID_ENV environment variable is not set"
            }
            val specFile = findSpecFile(nglAndroidEnvironment)

            extensions.getByType(LibraryAndroidComponentsExtension::class.java)
                .onVariants { variant ->
                    val generateNodesTask =
                        tasks.register(
                            "generate${variant.name}Nodes",
                            GenerateNodesTask::class.java
                        ) {
                            specs.set(specFile)
                        }
                    variant.sources.java?.addGeneratedSourceDirectory(
                        taskProvider = generateNodesTask,
                        wiredWith = { it.outputDirectory }
                    )
                }
        }
    }
}

private fun Project.findSpecFile(nglAndroidEnvironment: String): File {
    val locations = listOf(
        "$nglAndroidEnvironment/arm64-v8a/share/nopegl/nodes.specs",
        "$nglAndroidEnvironment/armeabi-v7a/share/nopegl/nodes.specs",
        "$nglAndroidEnvironment/x86/share/nopegl/nodes.specs",
        "$nglAndroidEnvironment/x86/share/nopegl/nodes.specs"
    )
    return locations.firstNotNullOfOrNull { file(it).takeIf(File::exists) }
        ?: error("No nodes.specs file found in ${locations.joinToString()}")
}
