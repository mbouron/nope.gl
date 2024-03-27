package org.nopeforge.nopegl.kotlin

import com.squareup.kotlinpoet.ClassName
import com.squareup.kotlinpoet.FileSpec
import com.squareup.kotlinpoet.FunSpec
import com.squareup.kotlinpoet.PropertySpec
import com.squareup.kotlinpoet.TypeSpec
import org.nopeforge.nopegl.specs.ChoiceSpec


data class ChoiceEnum(
    val packageName: String,
    val className: String,
    val entries: List<Entry>,
) {
    data class Entry(
        val className: String,
        val nativeName: String,
        val description: String,
    )

    companion object {
        const val NATIVE_VALUE_PROPERTY = "nativeValue"
        fun from(name: String, choices: List<ChoiceSpec>, packageName: String): ChoiceEnum {
            return ChoiceEnum(
                packageName = packageName,
                className = name.toCamelCase(true),
                entries = choices.map {
                    Entry(
                        className = it.name.toCamelCase(true),
                        nativeName = it.name,
                        description = it.desc
                    )
                }
            )
        }
    }
}

fun ChoiceEnum.toFileSpec(): FileSpec {
    val typeSpec = TypeSpec.enumBuilder(className)
        .primaryConstructor(
            FunSpec.constructorBuilder()
                .addParameter(ChoiceEnum.NATIVE_VALUE_PROPERTY, String::class)
                .build()
        )
        .addProperty(
            PropertySpec.builder(ChoiceEnum.NATIVE_VALUE_PROPERTY, String::class)
                .initializer(ChoiceEnum.NATIVE_VALUE_PROPERTY)
                .build()
        ).apply {
            entries.forEach { entry ->
                addEnumConstant(
                    entry.className,
                    TypeSpec.anonymousClassBuilder()
                        .addKdoc(entry.description)
                        .addSuperclassConstructorParameter("%S", entry.nativeName)
                        .build()
                )
            }
        }
        .build()
    return FileSpec.builder(ClassName(packageName, className))
        .addType(typeSpec)
        .build()
}