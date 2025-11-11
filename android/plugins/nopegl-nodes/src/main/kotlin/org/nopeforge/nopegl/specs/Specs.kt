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

package org.nopeforge.nopegl.specs

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

@Serializable
data class Specs(
    val types: List<TypeSpec>,
    val choices: Map<String, List<ChoiceSpec>>,
    val nodes: Map<String, NodeSpec>,
)

@Serializable
data class TypeSpec(
    val name: TypeName,
    val size: Int,
    val desc: String,
)

@Serializable
enum class TypeName {
    @SerialName("i32")
    I32,

    @SerialName("ivec2")
    IVec,

    @SerialName("ivec3")
    Ivec3,

    @SerialName("ivec4")
    Ivec4,

    @SerialName("bool")
    Bool,

    @SerialName("u32")
    U32,

    @SerialName("uvec2")
    UVec2,

    @SerialName("uvec3")
    UVec3,

    @SerialName("uvec4")
    UVec4,

    @SerialName("f64")
    F64,

    @SerialName("str")
    Str,

    @SerialName("data")
    Data,

    @SerialName("f32")
    F32,

    @SerialName("vec2")
    Vec2,

    @SerialName("vec3")
    Vec3,

    @SerialName("vec4")
    Vec4,

    @SerialName("mat4")
    Mat4,

    @SerialName("node")
    Node,

    @SerialName("node_list")
    NodeList,

    @SerialName("f64_list")
    F64List,

    @SerialName("node_dict")
    NodeDict,

    @SerialName("select")
    Select,

    @SerialName("flags")
    Flags,

    @SerialName("rational")
    Rational,
}

@Serializable
data class ChoiceSpec(
    val name: String,
    val desc: String,
)

@Serializable
data class NodeSpec(
    val file: String? = null,
    val params: List<ParamSpec>,
)

@Serializable
data class ParamSpec(
    val name: String,
    val type: TypeName,
    @SerialName("choices")
    val choicesType: String? = null,
    //val default: JsonElement? = null, // ignored for now
    val desc: String,
    val flags: List<FlagSpec> = emptyList(),
    @SerialName("node_types")
    val nodeTypes: List<String> = emptyList(),
) {
    val nullable: Boolean = !flags.contains(FlagSpec.NonNull)
    val canBeNode: Boolean = flags.contains(FlagSpec.Node)
    val mutable: Boolean = flags.contains(FlagSpec.Live)
}

@Serializable
enum class FlagSpec {
    @SerialName("live")
    Live,

    @SerialName("nonull")
    NonNull,

    @SerialName("filepath")
    Filepath,

    @SerialName("node")
    Node
}
