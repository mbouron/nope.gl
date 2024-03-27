package org.nopeforge.nopegl

import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject

const val PACKAGE_NAME = "org.nopeforge.nopegl"

fun sanitizedSpecs(jsonObject: JsonObject): JsonElement {
    val elements = jsonObject.toMutableMap().apply {
        val nodes = requireNotNull(get("nodes")).jsonObject

        // Replace string literal node with the correct json object
        set("nodes", buildJsonObject {
            nodes.map { (key, nodeElement) ->
                if (nodeElement is JsonPrimitive) {
                    nodes[nodeElement.content]?.let { element -> put(key, element) }
                } else {
                    put(key, nodeElement)
                }
            }
        })
    }
    return buildJsonObject {
        elements.forEach { (key, value) -> put(key, value) }
    }
}