/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

import android.hardware.HardwareBuffer
import android.os.ParcelFileDescriptor
import android.view.Surface
import org.nopeforge.Nope

/* Images drawn into surface, see canvas.c. */
internal class NGLAndroidCanvasImageQueue(
    width: Int,
    height: Int,
    format: Int,
    usage: Long,
    maxImages: Int,
) : AutoCloseable {
    private val queue: Long
    val surface: Surface
    private val images = HashMap<HardwareBuffer, Long>()

    /* A discarded buffer went back to the queue, so nothing must be drawn into it anymore. */
    var broken = false
        private set

    init {
        Nope.load()
        queue = nativeCreate(width, height, format, usage, maxImages)
        check(queue != 0L) { "Could not create the canvas image queue" }
        surface = checkNotNull(nativeGetSurface(queue))
    }

    /* Acquires the next image queued to surface, waiting for it to be queued if needed. */
    fun acquire(): NGLAndroidCanvasBuffer? {
        val fence = IntArray(1)
        val image = nativeAcquire(queue, ACQUIRE_TIMEOUT_MS, fence)
        if (image == 0L) return null
        val buffer = nativeGetHardwareBuffer(image)
        if (buffer == null) {
            nativeRelease(queue, image, fence[0])
            return null
        }
        images[buffer] = image
        return NGLAndroidCanvasBuffer(buffer, fence[0])
    }

    /*
     * Gives back a buffer returned by acquire(). The queue takes ownership of
     * fenceFd. The producer waits for it before drawing into the buffer again.
     */
    fun release(hardwareBuffer: HardwareBuffer, fenceFd: Int) {
        val image = images.remove(hardwareBuffer)
        hardwareBuffer.close()
        if (image == null) {
            if (fenceFd >= 0)
                ParcelFileDescriptor.adoptFd(fenceFd).close()
            return
        }
        if (fenceFd < -1)
            broken = true
        nativeRelease(queue, image, fenceFd)
    }

    override fun close() {
        surface.release()
        nativeClose(queue)
    }

    private external fun nativeCreate(width: Int, height: Int, format: Int, usage: Long, maxImages: Int): Long
    private external fun nativeGetSurface(queue: Long): Surface?
    private external fun nativeAcquire(queue: Long, timeoutMs: Int, fenceOut: IntArray): Long
    private external fun nativeGetHardwareBuffer(image: Long): HardwareBuffer?
    private external fun nativeRelease(queue: Long, image: Long, fenceFd: Int)
    private external fun nativeClose(queue: Long)

    private companion object {
        const val ACQUIRE_TIMEOUT_MS = 1000
    }
}
