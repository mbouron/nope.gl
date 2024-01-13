/*
 * Copyright 2024 Nope Forge
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

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