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
            val className = "NGL${name.toCamelCase(true)}"
            return ChoiceEnum(
                packageName = packageName,
                className = className,
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