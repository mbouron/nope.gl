import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.decodeFromJsonElement
import kotlinx.serialization.json.jsonObject
import org.nopeforge.nopegl.PACKAGE_NAME
import org.nopeforge.nopegl.kotlin.ChoiceEnum
import org.nopeforge.nopegl.kotlin.NodeClass
import org.nopeforge.nopegl.kotlin.toFileSpec
import org.nopeforge.nopegl.sanitizedSpecs
import org.nopeforge.nopegl.specs.Specs
import java.io.File


fun main() {
    val json = Json { ignoreUnknownKeys = true }
    Json::class.java.getResource("/nodes.specs")?.readText()?.let { text ->
        val jsonObject = json.parseToJsonElement(text).jsonObject
        val sanitizedJsonObject = sanitizedSpecs(jsonObject)
        json.decodeFromJsonElement<Specs>(sanitizedJsonObject)
    }?.let { specs ->
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
            .forEach { it.writeTo(File("generated")) }
    }
}
