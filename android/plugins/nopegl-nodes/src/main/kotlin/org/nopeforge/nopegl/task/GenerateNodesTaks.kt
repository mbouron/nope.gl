package org.nopeforge.nopegl.task

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.decodeFromJsonElement
import kotlinx.serialization.json.jsonObject
import org.gradle.api.DefaultTask
import org.gradle.api.file.DirectoryProperty
import org.gradle.api.file.RegularFileProperty
import org.gradle.api.tasks.CacheableTask
import org.gradle.api.tasks.InputFile
import org.gradle.api.tasks.OutputDirectory
import org.gradle.api.tasks.OutputFile
import org.gradle.api.tasks.PathSensitive
import org.gradle.api.tasks.PathSensitivity
import org.gradle.api.tasks.TaskAction
import org.nopeforge.nopegl.PACKAGE_NAME
import org.nopeforge.nopegl.kotlin.ChoiceEnum
import org.nopeforge.nopegl.kotlin.NodeClass
import org.nopeforge.nopegl.kotlin.toFileSpec
import org.nopeforge.nopegl.sanitizedSpecs
import org.nopeforge.nopegl.specs.Specs

@CacheableTask
internal abstract class GenerateNodesTask : DefaultTask() {
    @get:InputFile
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val specs: RegularFileProperty

    @get:OutputDirectory
    abstract val outputDirectory: DirectoryProperty

    @TaskAction
    fun generateNodes() {
        val specFile = specs.get().asFile
        val json = Json { ignoreUnknownKeys = true }
        val specs = specFile.readText().let { text ->
            val jsonObject = json.parseToJsonElement(text).jsonObject
            val sanitizedJsonObject = sanitizedSpecs(jsonObject)
            json.decodeFromJsonElement<Specs>(sanitizedJsonObject)
        }
        val choices = specs.choices.mapValues { (name, choiceSpecs) ->
            ChoiceEnum.from(name, choiceSpecs, PACKAGE_NAME)
        }
        val nodeClasses = specs.nodes
            .filterKeys { !it.startsWith("_") }
            .mapValues { (name, nodeSpec) ->
                NodeClass.from(name, nodeSpec, PACKAGE_NAME)
            }

        choices.values.map { it.toFileSpec() }
            .plus(nodeClasses.values.map { it.toFileSpec(choices) })
            .forEach { it.writeTo(outputDirectory.get().asFile) }
    }
}