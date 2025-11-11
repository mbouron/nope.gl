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

import com.android.build.api.variant.LibraryAndroidComponentsExtension
import org.gradle.api.Plugin
import org.gradle.api.Project
import org.nopeforge.nopegl.task.GenerateNodesTask
import java.io.File

class GenerateNodesPlugin : Plugin<Project> {
    override fun apply(target: Project) {
        with(target) {
            val nglAndroidEnvironment = requireNotNull(System.getenv("NGL_ANDROID_ENV")) {
                "NGL_ANDROID_ENV environment variable is not set"
            }
            val specFile = findSpecFile(nglAndroidEnvironment)

            extensions.getByType(LibraryAndroidComponentsExtension::class.java)
                .onVariants { variant ->
                    val generateNodesTask =
                        tasks.register(
                            "generate${variant.name}Nodes",
                            GenerateNodesTask::class.java
                        ) {
                            specs.set(specFile)
                        }
                    variant.sources.java?.addGeneratedSourceDirectory(
                        taskProvider = generateNodesTask,
                        wiredWith = { it.outputDirectory }
                    )
                }
        }
    }
}

private fun Project.findSpecFile(nglAndroidEnvironment: String): File {
    val locations = listOf(
        "$nglAndroidEnvironment/arm64-v8a/share/nopegl/nodes.specs",
        "$nglAndroidEnvironment/armeabi-v7a/share/nopegl/nodes.specs",
        "$nglAndroidEnvironment/x86_64/share/nopegl/nodes.specs",
    )
    return locations.firstNotNullOfOrNull { file(it).takeIf(File::exists) }
        ?: error("No nodes.specs file found in ${locations.joinToString()}")
}
