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
import android.util.Log

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
        private var hasImage = false
        private var currentWidth = 0
        private var currentHeight = 0

        private fun ensureRenderer() {
            val w = pendingWidth
            val h = pendingHeight
            if (renderer?.broken == false && w == currentWidth && h == currentHeight) return

            renderer?.close()
            renderNode?.discardDisplayList()
            hasImage = false

            renderer = NGLAndroidCanvasRenderer.Builder(w, h)
                .setBufferFormat(HardwareBuffer.RGBA_8888)
                .setUsageFlags(USAGE_FLAGS)
                .setMaxBuffers(MAX_BUFFERS)
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

        override fun unprepare() {
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
            ensureRenderer()

            val changed = callback.onPreDraw()
            if (hasImage && !changed) return

            val renderer = renderer ?: return
            val renderNode = renderNode ?: return
            val canvas = renderNode.beginRecording()
            callback.onDraw(canvas)
            renderNode.endRecording()

            hasImage = false
            val frame = renderer.draw()
            if (frame == null) {
                Log.w(TAG, "Could not draw the ${currentWidth}x${currentHeight} canvas")
                return
            }

            /*
             * The GPU may still read the previous buffer, possibly within the
             * current frame. nope.gl hands it back to the renderer with a fence
             * once it is done with it.
             */
            val buffer = frame.hardwareBuffer
            val info = HardwareBufferInfo(
                width = currentWidth,
                height = currentHeight,
                hardwareBuffer = buffer,
                acquireFenceFd = frame.acquireFenceFd,
            )
            hasImage = updateHardwareBufferInfo(info) { fd -> renderer.releaseBuffer(buffer, fd) } >= 0
            if (!hasImage)
                renderer.releaseBuffer(buffer, -1)
        }

        override fun release() {
            callback.onRelease()
            setHardwareBufferInfo(null)
            hasImage = false
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
         * Determines whether to draw a new frame or reuse the previous one.
         *
         * Called before each draw. If no previous frame is available for
         * reuse, the return value is ignored and a new frame is drawn.
         *
         * @return `true` to draw a new frame, or `false` to reuse the previous one.
         */
        fun onPreDraw(): Boolean = true
        fun onDraw(canvas: Canvas) = Unit
        fun onRelease() = Unit
        fun onDestroy() = Unit
    }

    companion object {
        private const val TAG = "NGLAndroidCanvas"
        private const val USAGE_FLAGS = USAGE_GPU_SAMPLED_IMAGE or USAGE_GPU_COLOR_OUTPUT
        // A canvas drawn by several nodes needs one buffer per draw until the frame is submitted.
        private const val MAX_BUFFERS = 8
    }
}
