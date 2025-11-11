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
import android.opengl.GLES11Ext.GL_TEXTURE_EXTERNAL_OES
import android.opengl.GLES32

class NGLCanvas(
    // TODO: don't expose the size here
    val width: Int,
    val height: Int,
    private val callback: Callback,
    private val tag: String? = null,
) {

    val node: NGLNode
        get() = customTextureNode

    private val customTextureCallback: NGLCustomTexture.Callback = object : NGLCustomTexture.Callback() {
        private var renderer: NGLCanvasRenderer? = null
        private var renderNode: RenderNode? = null
        private val values = IntArray(1)
        private var texture: Int = 0
        private var eglImage: Long = 0
        private var hardwareBuffer: HardwareBuffer? = null

        private fun createTexture() {
            GLES32.glGenTextures(1, values, 0)
            texture = values[0]

            GLES32.glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture)
            GLES32.glTexParameteri(
                GL_TEXTURE_EXTERNAL_OES,
                GLES32.GL_TEXTURE_MIN_FILTER,
                GLES32.GL_NEAREST,
            )
            GLES32.glTexParameteri(
                GL_TEXTURE_EXTERNAL_OES,
                GLES32.GL_TEXTURE_MAG_FILTER,
                GLES32.GL_LINEAR,
            )
            GLES32.glTexParameteri(
                GL_TEXTURE_EXTERNAL_OES,
                GLES32.GL_TEXTURE_WRAP_S,
                GLES32.GL_CLAMP_TO_EDGE,
            )
            GLES32.glTexParameteri(
                GL_TEXTURE_EXTERNAL_OES,
                GLES32.GL_TEXTURE_WRAP_T,
                GLES32.GL_CLAMP_TO_EDGE,
            )
            setTextureInfo(
                Info(
                    width = width,
                    height = height,
                    texture = texture,
                    target = GL_TEXTURE_EXTERNAL_OES
                )
            )
        }

        private fun releaseTexture() {
            setTextureInfo(null)
            GLES32.glDeleteTextures(1, intArrayOf(texture), 0)
            texture = 0
        }

        override fun init() {
            callback.onInit()
        }

        override fun prepare() {
            /* no-op */
        }

        override fun prefetch() {
            renderer = NGLCanvasRenderer.Builder(width, height)
                .setBufferFormat(HardwareBuffer.RGBA_8888)
                .setUsageFlags(USAGE_FLAGS)
                .setMaxBuffers(1)
                .build()

            renderNode = RenderNode(tag).also { node ->
                node.setPosition(0, 0, width, height)
                renderer?.setContentRoot(node)
            }

            createTexture()

            callback.onPrefetch()
        }

        override fun update(time: Double) {
            renderer ?: return
            callback.onUpdate(time)
        }

        override fun draw() {
            if (!callback.onPreDraw()) return
            if (eglImage != 0L) {
                EGLHelper.nativeReleaseEGLImage(eglImage)
            }
            eglImage = 0

            hardwareBuffer?.let { renderer?.releaseBuffer(it) }
            hardwareBuffer = null

            val renderer = renderer ?: return
            val renderNode = renderNode ?: return
            val canvas = renderNode.beginRecording()
            callback.onDraw(canvas)
            renderNode.endRecording()

            hardwareBuffer = renderer.draw()

            hardwareBuffer?.let {
                eglImage = EGLHelper.nativeCreateEGLImage(it)
            }

            if (eglImage != 0L) {
                EGLHelper.nativeUploadEGLImage(eglImage, texture)
            }
        }

        override fun release() {
            callback.onRelease()
            releaseTexture()
            if (eglImage != 0L) {
                EGLHelper.nativeReleaseEGLImage(eglImage)
            }
            eglImage = 0L
            hardwareBuffer?.let { renderer?.releaseBuffer(it) }
            hardwareBuffer = null
            renderer?.close()
            renderer = null
            renderNode?.discardDisplayList()
            renderNode = null
        }

        override fun uninit() {
            callback.onDestroy()
        }
    }

    private var customTextureNode = NGLCustomTexture(customTextureCallback)

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
