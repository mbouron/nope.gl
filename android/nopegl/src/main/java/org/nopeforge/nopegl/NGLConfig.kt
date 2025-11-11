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

class NGLConfig(
    @JvmField
    val backend: Int = 0,
    @JvmField
    val window: Long = 0,
    @JvmField
    val offscreen: Boolean = false,
    @JvmField
    val width: Int = 0,
    @JvmField
    val height: Int = 0,
    @JvmField
    val samples: Int = 0,
    @JvmField
    val swapInterval: Int = -1,
    @JvmField
    val disableDepth: Boolean = false,
    @JvmField
    val setSurfacePts: Boolean = false,
    @JvmField
    val clearColor: FloatArray = FloatArray(4),
    @JvmField
    val captureBuffer: ByteBuffer? = null,
    @JvmField
    val hud: Boolean = false,
    @JvmField
    val hudScale: Int = 1,
    @JvmField
    val debug: Boolean = false,
) {

    class Builder {
        private var backend: Int = 0
        private var window: Long = 0
        private var offscreen: Boolean = false
        private var width: Int = 0
        private var height: Int = 0
        private var samples: Int = 0
        private var swapInterval: Int = -1
        private var disableDepth: Boolean = false
        private var setSurfacePts: Boolean = false
        private var clearColor: FloatArray = FloatArray(4)
        private var captureBuffer: ByteBuffer? = null
        private var hud: Boolean = false
        private var hudScale: Int = 1
        private var debug: Boolean = false

        fun setBackend(backend: Int): Builder {
            this.backend = backend
            return this
        }

        fun setWindow(window: Long): Builder {
            this.window = window
            return this
        }

        fun setOffscreen(offscreen: Boolean): Builder {
            this.offscreen = offscreen
            return this
        }

        fun setWidth(width: Int): Builder {
            this.width = width
            return this
        }

        fun setHeight(height: Int): Builder {
            this.height = height
            return this
        }

        fun setSamples(samples: Int): Builder {
            this.samples = samples
            return this
        }

        fun setSwapInterval(swapInterval: Int): Builder {
            this.swapInterval = swapInterval
            return this
        }

        fun setDisableDepth(disableDepth: Boolean): Builder {
            this.disableDepth = disableDepth
            return this
        }

        fun setSetSurfacePts(setSurfacePts: Boolean): Builder {
            this.setSurfacePts = setSurfacePts
            return this
        }

        fun setClearColor(clearColor: FloatArray): Builder {
            this.clearColor = clearColor
            return this
        }

        fun setClearColor(r: Float, g: Float, b: Float, a: Float): Builder {
            this.clearColor = floatArrayOf(r, g, b, a)
            return this
        }

        fun setCaptureBuffer(captureBuffer: ByteBuffer?): Builder {
            this.captureBuffer = captureBuffer
            return this
        }

        fun setHud(hud: Boolean): Builder {
            this.hud = hud
            return this
        }

        fun setHudScale(hudScale: Int): Builder {
            this.hudScale = hudScale
            return this
        }

        fun setDebug(debug: Boolean): Builder {
            this.debug = debug
            return this
        }

        fun build(): NGLConfig {
            val config = NGLConfig(
                backend = backend,
                window = window,
                offscreen = offscreen,
                width = width,
                height = height,
                samples = samples,
                swapInterval = swapInterval,
                disableDepth = disableDepth,
                setSurfacePts = setSurfacePts,
                clearColor = clearColor,
                captureBuffer = captureBuffer,
                hud = hud,
                hudScale = hudScale,
            )
            return config
        }
    }

    companion object {
        const val BACKEND_AUTO = 0
        const val BACKEND_OPENGL = 1
        const val BACKEND_OPENGLES = 2
        const val BACKEND_VULKAN = 3
    }
}
