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

package org.nopeforge.nopemd

import android.net.Uri
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

class Player(
    private val uri: Uri,
    private val surface: Surface,
) {
    private val lock = Any()
    var currentTime: Double = Double.MIN_VALUE
        get() = synchronized(lock) { field }
        private set
    private var nativePtr: Long? = null
    private val handler: Handler
    private val handlerThread = HandlerThread("Player-${uri.hashCode()}").apply {
        start()
        handler = Handler(looper) { message ->
            when (message.what) {
                MSG_INIT -> onInit()
                MSG_START -> onStart()
                MSG_SEEK -> onSeek(message.obj as Double)
                MSG_DRAW -> {
                    val (time, latch) = message.obj as Pair<*, *>
                    onDraw(time as Double, latch as CountDownLatch?)
                }
                MSG_STOP -> onStop()
                MSG_RELEASE -> onRelease()
            }
            true
        }
        init()
    }

    private fun init() {
        val message = handler.obtainMessage(MSG_INIT)
        handler.sendMessage(message)
    }

    fun stop() {
        handler.removeCallbacksAndMessages(null)
        val message = handler.obtainMessage(MSG_STOP)
        handler.sendMessage(message)
    }

    fun start() {
        val message = handler.obtainMessage(MSG_START)
        handler.sendMessage(message)
    }

    fun seek(time: Double) {
        handler.removeMessages(MSG_DRAW)
        handler.removeMessages(MSG_SEEK)
        val message = handler.obtainMessage(MSG_SEEK, time)
        handler.sendMessage(message)
    }

    fun release() {
        handler.removeCallbacksAndMessages(null)
        val message = handler.obtainMessage(MSG_RELEASE)
        handler.sendMessageAtFrontOfQueue(message)
    }

    fun draw(time: Double, blocking: Boolean) {
        val latch = if (blocking) CountDownLatch(1) else null
        val message = handler.obtainMessage(MSG_DRAW, Pair(time, latch))
        handler.sendMessage(message)
        latch?.await(DRAW_TIMEOUT_IN_MS, TimeUnit.MILLISECONDS)
    }

    private fun onInit() {
        nativePtr = nativeInit(uri.toString(), surface)
        require(nativePtr != 0L) { "Failed to initialize player" }
    }

    private fun onStop() {
        nativePtr?.let { context ->
            nativeStop(context)
        }
    }

    private fun onStart() {
        nativePtr?.let { context ->
            val code = nativeStart(context)
            require(code == 0) { "Failed to start player" }
        }
    }

    private fun onSeek(position: Double) {
        nativePtr?.let { context ->
            val code = nativeSeek(context, position)
            if (code != 0) {
                Log.w("Player", "Failed to seek to $position")
            }
        }
    }

    private fun onDraw(time: Double, latch: CountDownLatch?) {
        nativePtr?.let { context ->
            val code = nativeDraw(context, time)
            if (code == 0) {
                synchronized(lock) {
                    currentTime = time
                }
            }
            latch?.countDown()
        }
    }

    private fun onRelease() {
        nativePtr?.let { context ->
            nativePtr = null
            nativeRelease(context)
        }
        handlerThread.quit()
    }

    private external fun nativeInit(url: String, surface: Surface): Long
    private external fun nativeStop(ptr: Long)
    private external fun nativeStart(ptr: Long): Int
    private external fun nativeSeek(ptr: Long, position: Double): Int
    private external fun nativeDraw(ptr: Long, time: Double): Int
    private external fun nativeRelease(ptr: Long)

    companion object {
        private const val MSG_INIT = 0x01
        private const val MSG_START = 0x02
        private const val MSG_SEEK = 0x03
        private const val MSG_DRAW = 0x04
        private const val MSG_STOP = 0x05
        private const val MSG_RELEASE = 0x06
        private const val DRAW_TIMEOUT_IN_MS = 1000L
    }
}
