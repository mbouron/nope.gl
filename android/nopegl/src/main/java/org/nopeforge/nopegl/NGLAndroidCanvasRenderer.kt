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

import android.graphics.HardwareRenderer
import android.graphics.RenderNode
import android.hardware.HardwareBuffer
import android.hardware.HardwareBuffer.USAGE_GPU_COLOR_OUTPUT
import android.hardware.HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE
import androidx.annotation.IntRange

internal data class NGLAndroidCanvasBuffer(
    val hardwareBuffer: HardwareBuffer,
    val acquireFenceFd: Int = -1,
)

internal class NGLAndroidCanvasRenderer
internal constructor(
    width: Int,
    height: Int,
    format: Int,
    usage: Long,
    maxBuffers: Int,
) {
    private val queue = NGLAndroidCanvasImageQueue(width, height, format, usage, maxBuffers)
    private val hardwareRenderer = HardwareRenderer().apply {
        isOpaque = false
        setSurface(queue.surface)
        start()
    }
    private var nextDrawTime = 0L

    /* The renderer cannot draw anymore and must be recreated. */
    val broken: Boolean get() = queue.broken

    fun close() {
        hardwareRenderer.stop()
        hardwareRenderer.destroy()
        queue.close()
    }

    fun setContentRoot(renderNode: RenderNode) {
        hardwareRenderer.setContentRoot(renderNode)
    }

    fun draw() : NGLAndroidCanvasBuffer? {
        if (queue.broken) return null

        /*
         * HWUI drops a frame if the latest vsync is less than 2ms after the
         * one of the previous frame. The vsync clock is shared with every
         * HWUI renderer of the process, including the application UI, so it
         * cannot be moved ahead of the current time.
         */
        val wait = nextDrawTime - System.nanoTime()
        if (wait > 0)
            Thread.sleep(wait / 1_000_000, (wait % 1_000_000).toInt())
        val request = hardwareRenderer.createRenderRequest().setVsyncTime(System.nanoTime())
        val result = request.syncAndDraw()
        if (result != HardwareRenderer.SYNC_OK && result != HardwareRenderer.SYNC_REDRAW_REQUESTED)
            return null
        val frame = queue.acquire()
        nextDrawTime = System.nanoTime() + MIN_FRAME_INTERVAL_NS
        return frame
    }

    /**
     * Give back a buffer returned by draw(). The renderer takes ownership of
     * fenceFd and only draws into the buffer again once it signals. A value
     * of -1 means the buffer can be reused immediately, a value below -1 that
     * it must be discarded.
     */
    fun releaseBuffer(hardwareBuffer: HardwareBuffer, fenceFd: Int) {
        queue.release(hardwareBuffer, fenceFd)
    }

    class Builder(private val width: Int, private val height: Int) {

        private var bufferFormat = HardwareBuffer.RGBA_8888
        private var maxBuffers = DEFAULT_NUM_BUFFERS
        private var usageFlags = DEFAULT_USAGE_FLAGS

        init {
            if (width <= 0 || height <= 0) {
                throw IllegalArgumentException(
                    "Invalid dimensions provided, width and height must be > 0. " +
                        "width: $width height: $height"
                )
            }
        }

        fun setBufferFormat(format: Int): Builder {
            bufferFormat = format
            return this
        }

        fun setMaxBuffers(@IntRange(from = 1, to = 64) numBuffers: Int): Builder {
            require(numBuffers > 0) { "Must have at least 1 buffer" }
            maxBuffers = numBuffers
            return this
        }

        fun setUsageFlags(usageFlags: Long): Builder {
            this.usageFlags = usageFlags or DEFAULT_USAGE_FLAGS
            return this
        }

        fun build(): NGLAndroidCanvasRenderer {
            return NGLAndroidCanvasRenderer(
                width,
                height,
                bufferFormat,
                usageFlags,
                maxBuffers,
            )
        }
    }

    internal companion object {
        private const val DEFAULT_NUM_BUFFERS = 3
        private const val DEFAULT_USAGE_FLAGS = USAGE_GPU_SAMPLED_IMAGE or USAGE_GPU_COLOR_OUTPUT
        private const val MIN_FRAME_INTERVAL_NS = 2_100_000L
    }
}

