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

import android.graphics.BlendMode
import android.graphics.Color
import android.graphics.HardwareBufferRenderer
import android.graphics.HardwareRenderer
import android.graphics.RenderNode
import android.hardware.HardwareBuffer
import android.hardware.HardwareBuffer.USAGE_GPU_COLOR_OUTPUT
import android.hardware.HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE
import android.media.Image
import android.media.ImageReader
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import androidx.annotation.IntRange
import androidx.annotation.RequiresApi
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

internal class NGLCanvasRenderer
internal constructor(
    width: Int,
    height: Int,
    format: Int,
    usage: Long,
    maxBuffers: Int,
) {
    private val impl: Impl =
        if (
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE && maxBuffers == 1
        ) {
            NGLCanvasRendererV34(width, height, format, usage)

        } else {
            NGLCanvasRendererV29(width, height, format, usage, maxBuffers)
        }

    fun close() {
        impl.close()
    }

    fun setContentRoot(renderNode: RenderNode) {
        impl.setContentRoot(renderNode)
    }

    fun draw() : HardwareBuffer? {
        return impl.draw()
    }

    fun releaseBuffer(hardwareBuffer: HardwareBuffer) {
        return impl.releaseBuffer(hardwareBuffer)
    }

    internal interface Impl {
        fun close()
        fun setContentRoot(renderNode: RenderNode)
        fun draw() : HardwareBuffer?
        fun releaseBuffer(hardwareBuffer: HardwareBuffer)
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

        fun build(): NGLCanvasRenderer {
            return NGLCanvasRenderer(
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
    }
}

internal class NGLCanvasRendererV29
internal constructor(
    private val width: Int,
    private val height: Int,
    private val format: Int,
    private val usage: Long,
    private val maxBuffers: Int,
) : NGLCanvasRenderer.Impl {
    private var contentRoot: RenderNode? = null
    private var handlerThread: HandlerThread = HandlerThread("NGLCanvasRendererV29").apply { start() }
    private var imageReader: ImageReader = createImageReader(handlerThread)
    private var hardwareRenderer: HardwareRenderer = createHardwareRenderer(imageReader)
    private val buffers = HashMap<HardwareBuffer, Image>()
    private var gotFrame = false
    private val lock = ReentrantLock()
    private val condition = lock.newCondition()

    private fun createImageReader(handlerThread: HandlerThread): ImageReader {
        return ImageReader.newInstance(width, height, format, maxBuffers, usage).apply {
            setOnImageAvailableListener({
                lock.withLock {
                    gotFrame = true
                    condition.signal()
                }
            }, Handler(handlerThread.looper))
        }
    }

    private fun createHardwareRenderer(imageReader: ImageReader): HardwareRenderer =
        HardwareRenderer().apply {
            isOpaque = false
            setSurface(imageReader.surface)
            start()
        }

    override fun close() {
        imageReader.close()
        hardwareRenderer.stop()
        hardwareRenderer.destroy()
        handlerThread.quitSafely()
    }

    override fun setContentRoot(renderNode: RenderNode) {
        contentRoot = renderNode
        hardwareRenderer.setContentRoot(contentRoot)
    }

    override fun draw() : HardwareBuffer? {
        val renderRequest = hardwareRenderer.createRenderRequest()
        val result = renderRequest.syncAndDraw()
        if (result != HardwareRenderer.SYNC_OK &&
            result != HardwareRenderer.SYNC_FRAME_DROPPED) {
            return null
        }

        lock.withLock {
            if (!gotFrame) {
                condition.await()
            }
            gotFrame = false
        }
        val image = imageReader.acquireNextImage()
        val buffer = image.hardwareBuffer
        if (buffer != null) {
            buffers[buffer] = image
        }

        return buffer
    }

    override fun releaseBuffer(hardwareBuffer: HardwareBuffer) {
        buffers.remove(hardwareBuffer)?.close()
        hardwareBuffer.close()
    }
}

@RequiresApi(Build.VERSION_CODES.UPSIDE_DOWN_CAKE)
internal class NGLCanvasRendererV34
internal constructor(
    private val width: Int,
    private val height: Int,
    private val format: Int,
    private val usage: Long,
) : NGLCanvasRenderer.Impl {
    private var rootNode: RenderNode = RenderNode("NGLCanvasRendererRoot")
    private var contentNode: RenderNode? = null
    private var hardwareBuffer = createHardwareBuffer()
    private var hardwareRenderer = createHardwareRenderer(hardwareBuffer)

    private fun createHardwareBuffer(): HardwareBuffer =
        HardwareBuffer.create(width, height, format, 1, usage)

    private fun createHardwareRenderer(hardwareBuffer: HardwareBuffer): HardwareBufferRenderer =
        HardwareBufferRenderer(hardwareBuffer).apply {
            setContentRoot(rootNode)
        }

    override fun close() {
        hardwareRenderer.close()
        hardwareBuffer.close()
    }

    override fun setContentRoot(renderNode: RenderNode) {
        contentNode = renderNode
    }

    override fun draw() : HardwareBuffer {
        rootNode.setPosition(0, 0, width, height)
        val canvas = rootNode.beginRecording()
        canvas.drawColor(Color.TRANSPARENT, BlendMode.CLEAR)
        contentNode?.let { canvas.drawRenderNode(it) }
        rootNode.endRecording()
        val renderRequest = hardwareRenderer.obtainRenderRequest()
        renderRequest.draw(Runnable::run) { result ->
            result.fence.apply {
                awaitForever()
                close()
            }
        }
        return hardwareBuffer
    }

    override fun releaseBuffer(hardwareBuffer: HardwareBuffer) {
    }
}
