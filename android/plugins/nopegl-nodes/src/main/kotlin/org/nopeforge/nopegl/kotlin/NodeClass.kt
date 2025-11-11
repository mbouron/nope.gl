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

package org.nopeforge.nopegl.kotlin

import com.squareup.kotlinpoet.*
import com.squareup.kotlinpoet.ParameterizedTypeName.Companion.parameterizedBy
import org.nopeforge.nopegl.NGLData
import org.nopeforge.nopegl.NGLIVec2
import org.nopeforge.nopegl.NGLIVec3
import org.nopeforge.nopegl.NGLIVec4
import org.nopeforge.nopegl.NGLMat4
import org.nopeforge.nopegl.NGLNode
import org.nopeforge.nopegl.NGLNodeOrValue
import org.nopeforge.nopegl.NGLNodeType
import org.nopeforge.nopegl.NGLRational
import org.nopeforge.nopegl.NGLUVec2
import org.nopeforge.nopegl.NGLUVec3
import org.nopeforge.nopegl.NGLUVec4
import org.nopeforge.nopegl.NGLVec2
import org.nopeforge.nopegl.NGLVec3
import org.nopeforge.nopegl.NGLVec4
import org.nopeforge.nopegl.specs.NodeSpec
import org.nopeforge.nopegl.specs.TypeName
import org.nopeforge.nopegl.specs.nodeType

data class NodeClass(
    val packageName: String,
    val className: String,
    val sourceFile: String?,
    val type: NGLNodeType,
    val parameters: List<Parameter>,
) {
    data class Parameter(
        val parameterName: String,
        val name: String,
        val type: TypeName,
        val description: String,
        val choicesType: String?,
        val nullable: Boolean,
        val canBeNode: Boolean,
        val live: Boolean,
        val addSetter: Boolean = true,
    ) {
        companion object {
            val Label = Parameter(
                parameterName = "label",
                name = "label",
                type = TypeName.Str,
                description = "label of the node",
                choicesType = null,
                nullable = true,
                canBeNode = false,
                live = false,
                addSetter = false
            )
        }
    }

    companion object {
        fun from(name: String, spec: NodeSpec, packageName: String): NodeClass {
            val type = requireNotNull(nodeType(name)) {
                "Unknown node type: $name"
            }
            val className = "NGL${name.toCamelCase(true)}"
            return NodeClass(
                packageName = packageName,
                className = className,
                sourceFile = spec.file,
                type = type,
                parameters = spec.params.map {
                    Parameter(
                        parameterName = it.name.toCamelCase(),
                        name = it.name,
                        type = it.type,
                        description = it.desc,
                        choicesType = it.choicesType,
                        nullable = it.nullable,
                        canBeNode = it.canBeNode,
                        live = it.mutable,
                    )
                } + Parameter.Label
            )
        }
    }
}

fun NodeClass.toFileSpec(choiceEnums: Map<String, ChoiceEnum>): FileSpec {
    return FileSpec.builder(packageName, className)
        .addType(toTypeSpec(choiceEnums))
        .build()
}

private fun NodeClass.toTypeSpec(choices: Map<String, ChoiceEnum>): TypeSpec {
    val parameterSpecs = parameters.associateWith { param ->
        param.toKotlinParameter(choices)
    }
    return TypeSpec.classBuilder(className)
        .superclass(NGLNode::class.asClassName())
        .addKdoc(
            CodeBlock.builder()
                .addStatement(if (sourceFile == null) "" else "file: $sourceFile")
                .apply {
                    parameters.forEach {
                        addStatement("@param ${it.parameterName} ${it.description} (live: ${it.live})")
                    }
                }
                .build()
        )
        .addSuperclassConstructorParameter("%T.${type.name}", type::class.asClassName())
        .primaryConstructor(
            FunSpec.constructorBuilder()
                .apply { parameterSpecs.values.forEach(::addParameter) }
                .build()
        )
        .addInitializerBlock(
            CodeBlock.builder()
                .apply {
                    parameterSpecs.forEach { (param, parameter) ->
                        add(
                            kotlinSetCall(
                                typeName = param.type,
                                name = param.name,
                                kotlinName = parameter.name,
                                nullable = param.nullable,
                                canBeNode = param.canBeNode,
                            )
                        )
                    }
                }
                .build()
        ).also {
            if (type == NGLNodeType.TIMERANGEFILTER) {
                it.addFunction(
                    FunSpec.builder("setRange")
                        .addParameter("startTime", Double::class)
                        .addParameter("endTime", Double::class)
                        .addCode(
                            CodeBlock.of(
                                "super.${NGLNode::setTimeRangeFilterRange.name}(startTime, endTime)"
                            )
                        ).build()
                )
            }
        }.addFunctions(parameterSpecs.filter { it.key.addSetter }
            .flatMap { (param, parameter) ->
                val block = kotlinSetCall(
                    typeName = param.type,
                    name = param.name,
                    kotlinName = parameter.name,
                    nullable = false,
                    canBeNode = param.canBeNode
                )
                val defaultFunction = block.let { setCall ->
                    FunSpec.builder("set${param.parameterName.toCamelCase(true)}")
                        .addKdoc(param)
                        .addParameter(
                            parameter
                                .toBuilder(type = parameter.type.copy(nullable = false))
                                .defaultValue(null)
                                .build()
                        )
                        .addCode(setCall)
                        .build()
                }

                when (param.type) {
                    TypeName.NodeDict -> nodeDictFunctions(
                        defaultFunction = defaultFunction,
                        block = block,
                        param = param,
                        parameter = parameter
                    )

                    TypeName.NodeList -> nodeListFunctions(
                        defaultFunction = defaultFunction,
                        block = block,
                        param = param,
                        parameter = parameter
                    )

                    else -> listOf(defaultFunction)
                }
            })
        .build()
}

private fun FunSpec.Builder.addKdoc(param: NodeClass.Parameter): FunSpec.Builder {
    return addKdoc(
        CodeBlock.builder()
            .addStatement(param.description)
            .run {
                if (param.live) {
                    addStatement("Once the node's scene is loaded, this parameter should be updated from a single thread.")
                } else {
                    addStatement("This parameter cannot be changed once the node's scene is loaded.")
                }
            }
            .build()
    )
}

private fun nodeListFunctions(
    defaultFunction: FunSpec,
    block: CodeBlock,
    param: NodeClass.Parameter,
    parameter: ParameterSpec,
) = listOf(
    defaultFunction,
    FunSpec.builder("set${param.parameterName.toCamelCase(true)}")
        .addParameter("nodes", NGLNode::class.asTypeName(), KModifier.VARARG)
        .addCode(
            CodeBlock.builder()
                .addStatement("val ${parameter.name} = nodes.toList()")
                .add(block)
                .build()
        )
        .build(),
    FunSpec.builder("swap${param.parameterName.toCamelCase(true)}")
        .addParameter("from", Int::class.asTypeName())
        .addParameter("to", Int::class.asTypeName())
        .addCode(CodeBlock.of("${NGLNode::swapElement.name}(%S, from, to)\n", param.parameterName))
        .build()
)

private fun nodeDictFunctions(
    defaultFunction: FunSpec,
    block: CodeBlock,
    param: NodeClass.Parameter,
    parameter: ParameterSpec,
) = listOf(
    defaultFunction,
    FunSpec.builder("set${param.parameterName.toCamelCase(true)}")
        .addParameter(
            "pairs",
            Pair::class.asTypeName().parameterizedBy(
                String::class.asTypeName(),
                NGLNode::class.asTypeName()
            ),
            KModifier.VARARG
        )
        .addCode(
            CodeBlock.builder()
                .addStatement("val ${parameter.name} = pairs.toMap()")
                .add(block)
                .build()
        )
        .build()
)

private fun kotlinSetCall(
    typeName: TypeName,
    name: String,
    kotlinName: String,
    nullable: Boolean,
    canBeNode: Boolean,
): CodeBlock {
    val propertyAccessor = if (canBeNode) {
        "$kotlinName.${NGLNodeOrValue.Value<*>::value.name}"
    } else {
        kotlinName
    }
    val setBlock = CodeBlock.builder()
        .apply {
            when (typeName) {
                TypeName.I32 -> {
                    addStatement("${NGLNode::setInt.name}(%S, $propertyAccessor)", name)
                }

                TypeName.IVec -> {
                    addStatement("${NGLNode::setIVec2.name}(%S, $propertyAccessor)", name)
                }

                TypeName.Ivec3 -> {
                    addStatement("${NGLNode::setIVec3.name}(%S, $propertyAccessor)", name)
                }

                TypeName.Ivec4 -> {
                    addStatement("${NGLNode::setIVec4.name}(%S, $propertyAccessor)", name)
                }

                TypeName.Bool -> {
                    addStatement("${NGLNode::setBoolean.name}(%S, $propertyAccessor)", name)
                }

                TypeName.U32 -> {
                    addStatement("${NGLNode::setUInt.name}(%S, $propertyAccessor)", name)
                }

                TypeName.UVec2 -> {
                    addStatement("${NGLNode::setUVec2.name}(%S, $propertyAccessor)", name)
                }

                TypeName.UVec3 -> {
                    addStatement("${NGLNode::setUVec3.name}(%S, $propertyAccessor)", name)
                }

                TypeName.UVec4 -> {
                    addStatement("${NGLNode::setUVec4.name}(%S, $propertyAccessor)", name)
                }

                TypeName.F64 -> {
                    addStatement("${NGLNode::setDouble.name}(%S, $propertyAccessor)", name)
                }

                TypeName.Str -> {
                    addStatement("${NGLNode::setString.name}(%S, $propertyAccessor)", name)
                }

                TypeName.Data -> {
                    addStatement("${NGLNode::setData.name}(%S, $propertyAccessor)", name)
                }

                TypeName.F32 -> {
                    addStatement("${NGLNode::setFloat.name}(%S, $propertyAccessor)", name)
                }

                TypeName.Vec2 -> {
                    addStatement("${NGLNode::setVec2.name}(%S, $propertyAccessor)", name)
                }

                TypeName.Vec3 -> {
                    addStatement("${NGLNode::setVec3.name}(%S, $propertyAccessor)", name)
                }

                TypeName.Vec4 -> {
                    addStatement("${NGLNode::setVec4.name}(%S, $propertyAccessor)", name)
                }

                TypeName.Mat4 -> {
                    addStatement("${NGLNode::setMat4.name}(%S, $propertyAccessor)", name)
                }

                TypeName.Node -> {
                    addStatement(
                        "${NGLNode::setNode.name}(%S, $propertyAccessor.${NGLNode::nativePtr.name})",
                        name
                    )
                }

                TypeName.NodeList -> {
                    addStatement("${NGLNode::addNodes.name}(%S, $propertyAccessor)", name)
                }

                TypeName.F64List -> {
                    addStatement("${NGLNode::addDoubles.name}(%S, $propertyAccessor)", name)
                }

                TypeName.NodeDict -> {
                    addStatement(propertyAccessor)
                        .beginControlFlow(".forEach { (key, value) ->")
                        .addStatement(
                            "${NGLNode::setDict.name}(%S, key, value.${NGLNode::nativePtr.name})",
                            name
                        )
                        .endControlFlow()
                }

                TypeName.Select -> {
                    addStatement(
                        "${NGLNode::setSelect.name}(%S, $propertyAccessor.${ChoiceEnum.NATIVE_VALUE_PROPERTY})",
                        name
                    )
                }

                TypeName.Flags -> {
                    addStatement("${NGLNode::setFlags.name}(%S, $propertyAccessor)", name)
                }

                TypeName.Rational -> {
                    addStatement("${NGLNode::setRational.name}(%S, $propertyAccessor)", name)
                }
            }
        }.build()
    return setBlock.let { block ->
        if (canBeNode) {
            CodeBlock.builder()
                .beginControlFlow("when ($kotlinName)")
                .beginControlFlow("is %T ->", NGLNodeOrValue.NGLNode::class.asTypeName())
                .addStatement(
                    "${NGLNode::setNode.name}(%S, $propertyAccessor.${NGLNode::nativePtr.name})",
                    name
                )
                .endControlFlow()
                .beginControlFlow("is %T ->", NGLNodeOrValue.Value::class.asTypeName())
                .add(block)
                .endControlFlow()
                .endControlFlow()
                .build()
        } else {
            block
        }
    }.let { block ->
        if (nullable) {
            CodeBlock.builder()
                .beginControlFlow("if ($kotlinName != null)")
                .add(block)
                .endControlFlow()
                .build()
        } else {
            block
        }
    }
}

private fun NodeClass.Parameter.toKotlinParameter(choiceEnums: Map<String, ChoiceEnum>): ParameterSpec {
    val parameterName = name.toCamelCase().let { if (it in KEYWORDS) "_$it" else it }
    val typeName = when (type) {
        TypeName.I32 -> Int::class.asTypeName()
        TypeName.IVec -> NGLIVec2::class.asTypeName()
        TypeName.Ivec3 -> NGLIVec3::class.asTypeName()
        TypeName.Ivec4 -> NGLIVec4::class.asTypeName()
        TypeName.Bool -> Boolean::class.asTypeName()
        TypeName.U32 -> UInt::class.asTypeName()
        TypeName.UVec2 -> NGLUVec2::class.asTypeName()
        TypeName.UVec3 -> NGLUVec3::class.asTypeName()
        TypeName.UVec4 -> NGLUVec4::class.asTypeName()
        TypeName.F64 -> Double::class.asTypeName()
        TypeName.Str -> String::class.asTypeName()
        TypeName.Data -> NGLData::class.asTypeName()
        TypeName.F32 -> Float::class.asTypeName()
        TypeName.Vec2 -> NGLVec2::class.asTypeName()
        TypeName.Vec3 -> NGLVec3::class.asTypeName()
        TypeName.Vec4 -> NGLVec4::class.asTypeName()
        TypeName.Mat4 -> NGLMat4::class.asTypeName()
        TypeName.Node -> NGLNode::class.asTypeName()

        TypeName.NodeList -> {
            List::class.asTypeName().parameterizedBy(NGLNode::class.asTypeName())

        }

        TypeName.F64List -> {
            List::class.asTypeName().parameterizedBy(Double::class.asTypeName())

        }

        TypeName.NodeDict -> {
            Map::class.asTypeName()
                .parameterizedBy(String::class.asTypeName(), NGLNode::class.asTypeName())

        }

        TypeName.Select -> {
            choiceEnums[choicesType]?.let { enum ->
                ClassName(enum.packageName, enum.className)
            } ?: String::class.asTypeName()
        }

        TypeName.Flags -> String::class.asTypeName()
        TypeName.Rational -> NGLRational::class.asTypeName()
    }

    val type = if (canBeNode) {
        NGLNodeOrValue::class.asTypeName().parameterizedBy(typeName)
    } else {
        typeName
    }
    return ParameterSpec.builder(parameterName, type.copy(nullable = nullable)).apply {
        if (nullable) {
            defaultValue("null")
        }
    }.build()
}
