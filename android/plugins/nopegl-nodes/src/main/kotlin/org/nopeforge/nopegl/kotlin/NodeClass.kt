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

import com.squareup.kotlinpoet.ClassName
import com.squareup.kotlinpoet.CodeBlock
import com.squareup.kotlinpoet.FileSpec
import com.squareup.kotlinpoet.FunSpec
import com.squareup.kotlinpoet.ParameterSpec
import com.squareup.kotlinpoet.ParameterizedTypeName.Companion.parameterizedBy
import com.squareup.kotlinpoet.TypeSpec
import com.squareup.kotlinpoet.asClassName
import com.squareup.kotlinpoet.asTypeName
import org.nopeforge.nopegl.NGLData
import org.nopeforge.nopegl.NGLIVec2
import org.nopeforge.nopegl.NGLIVec3
import org.nopeforge.nopegl.NGLIVec4
import org.nopeforge.nopegl.NGLMat4
import org.nopeforge.nopegl.NGLNodeOrValue
import org.nopeforge.nopegl.NGLNode
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
    )

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
                        canBeNode = it.canBeNode
                    )
                }
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
                        addStatement("@param ${it.parameterName} ${it.description}")
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
                        kotlinSetCall(
                            param.type,
                            param.name,
                            parameter.name,
                            param.nullable,
                            param.canBeNode
                        )?.let {
                            add(it)
                        }
                    }
                }
                .build()
        ).addFunctions(parameterSpecs.mapNotNull { (param, parameter) ->
            kotlinSetCall(
                param.type,
                param.name,
                parameter.name,
                false,
                param.canBeNode
            )?.let { setCall ->
                FunSpec.builder("set${param.parameterName.toCamelCase(true)}")
                    .addParameter(
                        parameter
                            .toBuilder(type = parameter.type.copy(nullable = false))
                            .defaultValue(null)
                            .build()
                    )
                    .addCode(setCall)
                    .build()
            }
        })
        .build()
}

private fun kotlinSetCall(
    typeName: TypeName,
    name: String,
    kotlinName: String,
    nullable: Boolean,
    canBeNode: Boolean,
): CodeBlock? {
    val propertyAccessor = if (canBeNode) {
        "$kotlinName.${NGLNodeOrValue.Value<*>::value.name}"
    } else {
        kotlinName
    }
    val setBlock = when (typeName) {
        TypeName.I32 -> CodeBlock.of("${NGLNode::setInt.name}(%S, $propertyAccessor)\n", name)
        TypeName.IVec -> CodeBlock.of("${NGLNode::setIVec2.name}(%S, $propertyAccessor)\n", name)
        TypeName.Ivec3 -> CodeBlock.of("${NGLNode::setIVec3.name}(%S, $propertyAccessor)\n", name)
        TypeName.Ivec4 -> CodeBlock.of("${NGLNode::setIVec4.name}(%S, $propertyAccessor)\n", name)
        TypeName.Bool -> CodeBlock.of("${NGLNode::setBoolean.name}(%S, $propertyAccessor)\n", name)
        TypeName.U32 -> CodeBlock.of("${NGLNode::setUInt.name}(%S, $propertyAccessor)\n", name)
        TypeName.UVec2 -> CodeBlock.of("${NGLNode::setUVec2.name}(%S, $propertyAccessor)\n", name)
        TypeName.UVec3 -> CodeBlock.of("${NGLNode::setUVec3.name}(%S, $propertyAccessor)\n", name)
        TypeName.UVec4 -> CodeBlock.of("${NGLNode::setUVec4.name}(%S, $propertyAccessor)\n", name)
        TypeName.F64 -> CodeBlock.of("${NGLNode::setDouble.name}(%S, $propertyAccessor)\n", name)
        TypeName.Str -> CodeBlock.of("${NGLNode::setString.name}(%S, $propertyAccessor)\n", name)
        TypeName.Data -> CodeBlock.of("${NGLNode::setData.name}(%S, $propertyAccessor)\n", name)
        TypeName.F32 -> CodeBlock.of("${NGLNode::setFloat.name}(%S, $propertyAccessor)\n", name)
        TypeName.Vec2 -> CodeBlock.of("${NGLNode::setVec2.name}(%S, $propertyAccessor)\n", name)
        TypeName.Vec3 -> CodeBlock.of("${NGLNode::setVec3.name}(%S, $propertyAccessor)\n", name)
        TypeName.Vec4 -> CodeBlock.of("${NGLNode::setVec4.name}(%S, $propertyAccessor)\n", name)
        TypeName.Mat4 -> CodeBlock.of("${NGLNode::setMat4.name}(%S, $propertyAccessor)\n", name)
        TypeName.Node -> CodeBlock.of(
            "${NGLNode::setNode.name}(%S, $propertyAccessor.${NGLNode::nativePtr.name})\n",
            name
        )

        TypeName.NodeList -> CodeBlock.of("${NGLNode::addNodes.name}(%S, $propertyAccessor)\n", name)
        TypeName.F64List -> CodeBlock.of("${NGLNode::addDoubles.name}(%S, $propertyAccessor)\n", name)

        TypeName.NodeDict -> {
            CodeBlock.builder()
                .beginControlFlow("$propertyAccessor.forEach { (key, value) ->")
                .addStatement("${NGLNode::setDict.name}(%S, key, value.${NGLNode::nativePtr.name})", name)
                .endControlFlow()
                .build()
        }

        TypeName.Select -> CodeBlock.of(
            "${NGLNode::setSelect.name}(%S, $propertyAccessor.${ChoiceEnum.NATIVE_VALUE_PROPERTY})\n",
            name
        )

        TypeName.Flags -> CodeBlock.of("${NGLNode::setFlags.name}(%S, $propertyAccessor)\n", name)
        TypeName.Rational -> CodeBlock.of(
            "${NGLNode::setRational.name}(%S, $propertyAccessor)\n",
            name
        )
    }
    return setBlock?.let {
        val block = CodeBlock.builder().apply {
            if (canBeNode) {
                beginControlFlow("when ($kotlinName) {")
                    .add("is %T ->", NGLNodeOrValue.NGLNode::class.asTypeName())
                    .add(
                        CodeBlock.of(
                            "${NGLNode::setNode.name}(%S, $propertyAccessor.${NGLNode::nativePtr.name})\n",
                            name
                        )
                    )
                    .add("is %T ->", NGLNodeOrValue.Value::class.asTypeName())
                    .add(setBlock)
                    .endControlFlow()
            } else {
                add(setBlock)
            }
        }.build()
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

    return ParameterSpec.builder(
        parameterName, if (canBeNode) {
            NGLNodeOrValue::class.asTypeName().parameterizedBy(typeName)
        } else {
            typeName
        }.copy(nullable = nullable)
    ).apply {
        if (nullable) {
            defaultValue("null")
        }
    }.build()
}

