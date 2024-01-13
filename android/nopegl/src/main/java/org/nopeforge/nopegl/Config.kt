/*
 * Copyright 2023-2024 Nope Forge
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

import java.nio.ByteBuffer

class Config {
    @JvmField
    var backend : Int = 0
    @JvmField
    var window: Long = 0
    @JvmField
    var offscreen : Boolean = false
    @JvmField
    var width : Int = 0
    @JvmField
    var height : Int = 0
    @JvmField
    var samples : Int = 0
    @JvmField
    var setSurfacePts : Boolean = false
    @JvmField
    var clearColor: FloatArray = FloatArray(4)
    @JvmField
    var captureBuffer: ByteBuffer? = null
    @JvmField
    var hud : Boolean = false
    @JvmField
    var hudScale : Int = 1

    companion object {
        const val BACKEND_AUTO = 0
        const val BACKEND_OPENGL = 1
        const val BACKEND_OPENGLES = 2
        const val BACKEND_VULKAN = 3
    }
}
