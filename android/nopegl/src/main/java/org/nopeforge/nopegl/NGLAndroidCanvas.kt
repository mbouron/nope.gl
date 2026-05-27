/*
 * Copyright 2023-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

import android.graphics.Canvas
import android.graphics.RenderNode
import android.hardware.HardwareBuffer
import android.hardware.HardwareBuffer.USAGE_GPU_COLOR_OUTPUT
import android.hardware.HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE

class NGLAndroidCanvas(
    width: Int,
    height: Int,
    private val callback: Callback,
    private val tag: String? = null,
    minFilter: NGLFilter = NGLFilter.Linear,
    magFilter: NGLFilter = NGLFilter.Linear,
) {

    @Volatile
    private var pendingWidth: Int = width.coerceAtLeast(1)

    @Volatile
    private var pendingHeight: Int = height.coerceAtLeast(1)

    /** Current target size */
    val width: Int get() = pendingWidth
    val height: Int get() = pendingHeight

    fun resize(width: Int, height: Int) {
        pendingWidth = width.coerceAtLeast(1)
        pendingHeight = height.coerceAtLeast(1)
    }

    val node: NGLNode
        get() = customTextureNode

    private val customTextureCallback: NGLCustomTexture.Callback = object : NGLCustomTexture.Callback() {
        private var renderer: NGLAndroidCanvasRenderer? = null
        private var renderNode: RenderNode? = null
        private var hardwareBuffer: HardwareBuffer? = null
        private var currentWidth = 0
        private var currentHeight = 0

        private fun ensureRenderer() {
            val w = pendingWidth
            val h = pendingHeight
            if (renderer != null && w == currentWidth && h == currentHeight) return

            renderer?.close()
            renderNode?.discardDisplayList()

            renderer = NGLAndroidCanvasRenderer.Builder(w, h)
                .setBufferFormat(HardwareBuffer.RGBA_8888)
                .setUsageFlags(USAGE_FLAGS)
                .setMaxBuffers(1)
                .build()

            renderNode = RenderNode(tag).also { node ->
                node.setPosition(0, 0, w, h)
                renderer?.setContentRoot(node)
            }

            currentWidth = w
            currentHeight = h
        }

        override fun init() {
            callback.onInit()
        }

        override fun prepare() {
            /* no-op */
        }

        override fun prefetch() {
            ensureRenderer()
            callback.onPrefetch()
        }

        override fun update(time: Double) {
            renderer ?: return
            callback.onUpdate(time)
        }

        override fun draw() {
            if (!callback.onPreDraw()) return

            setHardwareBufferInfo(null)
            hardwareBuffer?.let { renderer?.releaseBuffer(it) }
            hardwareBuffer = null

            ensureRenderer()

            val renderer = renderer ?: return
            val renderNode = renderNode ?: return
            val canvas = renderNode.beginRecording()
            callback.onDraw(canvas)
            renderNode.endRecording()

            val frame = renderer.draw() ?: return
            hardwareBuffer = frame.hardwareBuffer

            setHardwareBufferInfo(
                HardwareBufferInfo(
                    width = currentWidth,
                    height = currentHeight,
                    hardwareBuffer = frame.hardwareBuffer,
                    acquireFenceFd = frame.acquireFenceFd,
                )
            )
        }

        override fun release() {
            callback.onRelease()
            setHardwareBufferInfo(null)
            hardwareBuffer?.let { renderer?.releaseBuffer(it) }
            hardwareBuffer = null
            renderer?.close()
            renderer = null
            renderNode?.discardDisplayList()
            renderNode = null
            currentWidth = 0
            currentHeight = 0
        }

        override fun uninit() {
            callback.onDestroy()
        }
    }

    private var customTextureNode = NGLCustomTexture(customTextureCallback).apply {
        setMinFilter(minFilter)
        setMagFilter(magFilter)
    }

    interface Callback {
        fun onInit() = Unit
        fun onPrefetch() = Unit
        fun onUpdate(time: Double) = Unit

        /**
         * @return true to draw a frame, false otherwise
         */
        fun onPreDraw(): Boolean = true
        fun onDraw(canvas: Canvas) = Unit
        fun onRelease() = Unit
        fun onDestroy() = Unit
    }

    companion object {
        private const val USAGE_FLAGS = USAGE_GPU_SAMPLED_IMAGE or USAGE_GPU_COLOR_OUTPUT
    }
}
