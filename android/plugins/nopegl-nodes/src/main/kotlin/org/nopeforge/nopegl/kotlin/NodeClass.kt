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
import org.nopeforge.nopegl.Data
import org.nopeforge.nopegl.IVec2
import org.nopeforge.nopegl.IVec3
import org.nopeforge.nopegl.IVec4
import org.nopeforge.nopegl.Mat4
import org.nopeforge.nopegl.NodeOrValue
import org.nopeforge.nopegl.Node
import org.nopeforge.nopegl.NodeType
import org.nopeforge.nopegl.Rational
import org.nopeforge.nopegl.UVec2
import org.nopeforge.nopegl.UVec3
import org.nopeforge.nopegl.UVec4
import org.nopeforge.nopegl.Vec2
import org.nopeforge.nopegl.Vec3
import org.nopeforge.nopegl.Vec4
import org.nopeforge.nopegl.specs.NodeSpec
import org.nopeforge.nopegl.specs.TypeName
import org.nopeforge.nopegl.specs.nodeType

data class NodeClass(
    val packageName: String,
    val className: String,
    val sourceFile: String?,
    val type: NodeType,
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
            return NodeClass(
                packageName = packageName,
                className = name.toCamelCase(true),
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
        .superclass(Node::class.asClassName())
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
        "$kotlinName.${NodeOrValue.Value<*>::value.name}"
    } else {
        kotlinName
    }
    val setBlock = when (typeName) {
        TypeName.I32 -> CodeBlock.of("${Node::setInt.name}(%S, $propertyAccessor)\n", name)
        TypeName.IVec -> CodeBlock.of("${Node::setIVec2.name}(%S, $propertyAccessor)\n", name)
        TypeName.Ivec3 -> CodeBlock.of("${Node::setIVec3.name}(%S, $propertyAccessor)\n", name)
        TypeName.Ivec4 -> CodeBlock.of("${Node::setIVec4.name}(%S, $propertyAccessor)\n", name)
        TypeName.Bool -> CodeBlock.of("${Node::setBoolean.name}(%S, $propertyAccessor)\n", name)
        TypeName.U32 -> CodeBlock.of("${Node::setUInt.name}(%S, $propertyAccessor)\n", name)
        TypeName.UVec2 -> CodeBlock.of("${Node::setUVec2.name}(%S, $propertyAccessor)\n", name)
        TypeName.UVec3 -> CodeBlock.of("${Node::setUVec3.name}(%S, $propertyAccessor)\n", name)
        TypeName.UVec4 -> CodeBlock.of("${Node::setUVec4.name}(%S, $propertyAccessor)\n", name)
        TypeName.F64 -> CodeBlock.of("${Node::setDouble.name}(%S, $propertyAccessor)\n", name)
        TypeName.Str -> CodeBlock.of("${Node::setString.name}(%S, $propertyAccessor)\n", name)
        TypeName.Data -> CodeBlock.of("${Node::setData.name}(%S, $propertyAccessor)\n", name)
        TypeName.F32 -> CodeBlock.of("${Node::setFloat.name}(%S, $propertyAccessor)\n", name)
        TypeName.Vec2 -> CodeBlock.of("${Node::setVec2.name}(%S, $propertyAccessor)\n", name)
        TypeName.Vec3 -> CodeBlock.of("${Node::setVec3.name}(%S, $propertyAccessor)\n", name)
        TypeName.Vec4 -> CodeBlock.of("${Node::setVec4.name}(%S, $propertyAccessor)\n", name)
        TypeName.Mat4 -> CodeBlock.of("${Node::setMat4.name}(%S, $propertyAccessor)\n", name)
        TypeName.Node -> CodeBlock.of(
            "${Node::setNode.name}(%S, $propertyAccessor.${Node::nativePtr.name})\n",
            name
        )

        TypeName.NodeList -> CodeBlock.of("${Node::addNodes.name}(%S, $propertyAccessor)\n", name)
        TypeName.F64List -> CodeBlock.of("${Node::addDoubles.name}(%S, $propertyAccessor)\n", name)

        TypeName.NodeDict -> {
            CodeBlock.builder()
                .beginControlFlow("$propertyAccessor.forEach { (key, value) ->")
                .addStatement("${Node::setDict.name}(%S, key, value.${Node::nativePtr.name})", name)
                .endControlFlow()
                .build()
        }

        TypeName.Select -> CodeBlock.of(
            "${Node::setSelect.name}(%S, $propertyAccessor.${ChoiceEnum.NATIVE_VALUE_PROPERTY})\n",
            name
        )

        TypeName.Flags -> CodeBlock.of("${Node::setFlags.name}(%S, $propertyAccessor)\n", name)
        TypeName.Rational -> CodeBlock.of(
            "${Node::setRational.name}(%S, $propertyAccessor)\n",
            name
        )

        else -> null
    }
    return setBlock?.let {
        val block = CodeBlock.builder().apply {
            if (canBeNode) {
                beginControlFlow("when ($kotlinName) {")
                    .add("is %T ->", NodeOrValue.Node::class.asTypeName())
                    .add(
                        CodeBlock.of(
                            "${Node::setNode.name}(%S, $propertyAccessor.${Node::nativePtr.name})\n",
                            name
                        )
                    )
                    .add("is %T ->", NodeOrValue.Value::class.asTypeName())
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
        TypeName.IVec -> IVec2::class.asTypeName()
        TypeName.Ivec3 -> IVec3::class.asTypeName()
        TypeName.Ivec4 -> IVec4::class.asTypeName()
        TypeName.Bool -> Boolean::class.asTypeName()
        TypeName.U32 -> UInt::class.asTypeName()
        TypeName.UVec2 -> UVec2::class.asTypeName()
        TypeName.UVec3 -> UVec3::class.asTypeName()
        TypeName.UVec4 -> UVec4::class.asTypeName()
        TypeName.F64 -> Double::class.asTypeName()
        TypeName.Str -> String::class.asTypeName()
        TypeName.Data -> Data::class.asTypeName()
        TypeName.F32 -> Float::class.asTypeName()
        TypeName.Vec2 -> Vec2::class.asTypeName()
        TypeName.Vec3 -> Vec3::class.asTypeName()
        TypeName.Vec4 -> Vec4::class.asTypeName()
        TypeName.Mat4 -> Mat4::class.asTypeName()
        TypeName.Node -> Node::class.asTypeName()

        TypeName.NodeList -> {
            List::class.asTypeName().parameterizedBy(Node::class.asTypeName())

        }

        TypeName.F64List -> {
            List::class.asTypeName().parameterizedBy(Double::class.asTypeName())

        }

        TypeName.NodeDict -> {
            Map::class.asTypeName()
                .parameterizedBy(String::class.asTypeName(), Node::class.asTypeName())

        }

        TypeName.Select -> {
            choiceEnums[choicesType]?.let { enum ->
                ClassName(enum.packageName, enum.className)
            } ?: String::class.asTypeName()
        }

        TypeName.Flags -> String::class.asTypeName()
        TypeName.Rational -> Rational::class.asTypeName()
    }

    return ParameterSpec.builder(
        parameterName, if (canBeNode) {
            NodeOrValue::class.asTypeName().parameterizedBy(typeName)
        } else {
            typeName
        }.copy(nullable = nullable)
    ).apply {
        if (nullable) {
            defaultValue("null")
        }
    }.build()
}

