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

import android.content.Context
import android.graphics.PointF
import android.view.Surface
import java.nio.ByteBuffer

class NGLContext {
    private var nativePtr: Long
    private var captureBuffer: ByteBuffer? = null

    init {
        nativePtr = nativeCreate()
        if (nativePtr == 0L)
            throw OutOfMemoryError()
    }

    fun configure(config: NGLConfig): Int {
        return nativeConfigure(nativePtr, config)
    }

    fun resize(width: Int, height: Int): Int {
        return nativeResize(nativePtr, width, height)
    }

    fun setScene(scene: NGLScene): Int {
        return nativeSetScene(nativePtr, scene.nativePtr)
    }

    @Deprecated("Use resetScene(clear: Boolean) instead", ReplaceWith("resetScene(true)"))
    fun resetScene(): Int {
        return resetScene(true)
    }

    fun resetScene(clear: Boolean): Int {
        return nativeResetScene(nativePtr, clear)
    }

    fun draw(time: Double): Int {
        return nativeDraw(nativePtr, time)
    }

    fun update(time: Double): Int {
        return nativeUpdate(nativePtr, time)
    }

    fun setCaptureBuffer(buffer: ByteBuffer): Int {
        captureBuffer = buffer
        return nativeSetCaptureBuffer(nativePtr, buffer)
    }

    fun getIntersectingNodes(point: PointF): List<NGLNode> {
        val pointers = nativeIntersectingNodes(nativePtr, point.x, point.y)
            ?.toList()
            .orEmpty()
        return pointers.map { pointer -> NGLNode(pointer, false) }
    }

    fun release() {
        if (nativePtr != 0L) {
            nativeRelease(nativePtr)
            nativePtr = 0
        }
    }

    companion object {
        private var logger: NGLLogger? = null

        init {
            System.loadLibrary("lcms2")
            System.loadLibrary("avutil")
            System.loadLibrary("avcodec")
            System.loadLibrary("avformat")
            System.loadLibrary("avdevice")
            System.loadLibrary("swresample")
            System.loadLibrary("swscale")
            System.loadLibrary("avfilter")
            System.loadLibrary("png16")
            System.loadLibrary("freetype")
            System.loadLibrary("fribidi")
            System.loadLibrary("harfbuzz")
            System.loadLibrary("nopemd")
            System.loadLibrary("nopegl")
            System.loadLibrary("nopegl_native")
        }

        @JvmStatic
        @Deprecated(
            "Use init(context: Context?) instead. To handle logging use NGLLogger",
            ReplaceWith("init(context, logLevel, { level, tag, message ->  })")
        )
        fun init(context: Context?, logLevel: String = "info") {
            nativeInit(context, logLevel)
        }

        @JvmStatic
        fun init(
            context: Context?,
            level: LogLevel,
            onLog: (level: LogLevel, tag: String, message: String) -> Unit,
        ) {
            nativeInit(context, null)
            logger?.release()
            logger = NGLLogger(level, onLog)
        }

        fun release() {
            logger?.release()
            logger = null
        }

        @JvmStatic
        fun createNativeWindow(surface: Surface?): Long {
            return nativeCreateNativeWindow(surface)
        }

        @JvmStatic
        fun releaseNativeWindow(nativePtr: Long) {
            nativeReleaseNativeWindow(nativePtr)
        }

        @JvmStatic
        private external fun nativeInit(context: Context?, logLevel: String?)

        @JvmStatic
        private external fun nativeCreateNativeWindow(surface: Any?): Long

        @JvmStatic
        private external fun nativeReleaseNativeWindow(nativePtr: Long)
    }

    /* Scene */
    private external fun nativeSetScene(nativePtr: Long, sceneNativePtr: Long): Int
    private external fun nativeResetScene(nativePtr: Long, clear: Boolean): Int

    /* Context */
    private external fun nativeCreate(): Long
    private external fun nativeConfigure(nativePtr: Long, config: NGLConfig): Int
    private external fun nativeResize(nativePtr: Long, width: Int, height: Int): Int
    private external fun nativeDraw(nativePtr: Long, time: Double): Int

    private external fun nativeUpdate(nativePtr: Long, time: Double): Int
    private external fun nativeSetCaptureBuffer(nativePtr: Long, buffer: ByteBuffer): Int
    private external fun nativeRelease(nativePtr: Long)
    private external fun nativeIntersectingNodes(nativePtr: Long, x: Float, y: Float): LongArray?
}
