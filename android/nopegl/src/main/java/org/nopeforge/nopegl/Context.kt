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
import java.nio.ByteBuffer

class Context {
    private var nativePtr: Long
    private var captureBuffer: ByteBuffer? = null

    init {
        nativePtr = nativeCreate()
        if (nativePtr == 0L)
            throw OutOfMemoryError()
    }

    fun configure(config: Config): Int {
        return nativeConfigure(nativePtr, config)
    }

    fun resize(width: Int, height: Int): Int {
        return nativeResize(nativePtr, width, height)
    }

    fun setScene(scene: Scene): Int {
        return nativeSetScene(nativePtr, scene.nativePtr)
    }

    fun resetScene(): Int {
        return nativeResetScene(nativePtr)
    }

    fun draw(time: Double): Int {
        return nativeDraw(nativePtr, time)
    }

    fun setCaptureBuffer(buffer : ByteBuffer) : Int {
        captureBuffer = buffer
        return nativeSetCaptureBuffer(nativePtr, buffer)
    }

    fun finalize() {
        nativeRelease(nativePtr)
        nativePtr = 0
    }

    companion object {
        init {
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
        external fun nativeInit(context: Context?)

        @JvmStatic
        external fun nativeCreateNativeWindow(surface: Any?): Long

        @JvmStatic
        external fun nativeReleaseNativeWindow(nativePtr: Long)
    }

    /* Scene */
    private external fun nativeSetScene(nativePtr: Long, sceneNativePtr: Long): Int
    private external fun nativeResetScene(nativePtr: Long): Int

    /* NodeGL */
    private external fun nativeCreate(): Long
    private external fun nativeConfigure(nativePtr: Long, config: Config): Int
    private external fun nativeResize(
        nativePtr: Long,
        width: Int,
        height: Int,
    ): Int

    private external fun nativeDraw(nativePtr: Long, time: Double): Int
    private external fun nativeSetCaptureBuffer(nativePtr: Long, buffer: ByteBuffer) : Int
    private external fun nativeRelease(nativePtr: Long)
}
