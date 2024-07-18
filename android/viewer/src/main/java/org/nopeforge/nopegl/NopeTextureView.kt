/*
 * Copyright 2024 Nope Forge
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
import android.graphics.SurfaceTexture
import android.os.Handler
import android.os.HandlerThread
import android.os.Message
import android.view.Choreographer
import android.view.Surface
import android.view.TextureView


class NopeTextureView(
    context: Context,
) : TextureView(context), TextureView.SurfaceTextureListener, Choreographer.FrameCallback {
    @Volatile
    private var time: Double = 0.0
    private var duration: Double = 1.0
    private var frameRate: NGLRational = NGLRational(num = 60, den = 1)
    private var clockOffset = -1L
    private var gotFirstFrame = false
    private var frameIndex = 0L
    private var paused = false

    private var nativeWindow: Long = 0
    private var ctx: NGLContext? = null

    private var handlerThread: HandlerThread
    private var handler: Handler

    @Volatile
    private var stopped = false

    init {
        surfaceTextureListener = this

        val surfaceTexture = SurfaceTexture(false)
        surfaceTexture.setDefaultBufferSize(width, height)
        setSurfaceTexture(surfaceTexture)

        handlerThread = HandlerThread("NopeTextureView").apply {
            start()
            handler = object : Handler(looper) {
                override fun handleMessage(msg: Message) {
                    when (msg.what) {
                        MSG_INIT -> {
                            setupContext()
                        }

                        MSG_START -> {
                            stopped = false
                            paused = false
                            resetClock()
                            Choreographer.getInstance().postFrameCallback(this@NopeTextureView)
                        }

                        MSG_PAUSE -> {
                            paused = true
                        }

                        MSG_STOP -> {
                            stopped = true
                            paused = true
                            resetClock()
                            Choreographer.getInstance().removeFrameCallback(this@NopeTextureView)
                        }

                        MSG_SET_SCENE -> {
                            val scene = msg.obj as NGLScene?
                            scene?.let {
                                ctx?.setScene(scene)
                                duration = scene.duration
                                gotFirstFrame = false
                                resetClock()
                            }
                        }

                        MSG_STEP -> {
                            val step = msg.obj as Int
                            val max = Math.round(duration * frameRate.num / frameRate.den)
                            frameIndex += step
                            frameIndex = frameIndex.coerceAtLeast(0)
                            frameIndex = frameIndex.coerceAtMost(max)
                            paused = true
                            resetClock()
                        }

                        MSG_RESIZE -> {
                            ctx?.resize(msg.arg1, msg.arg2)
                        }

                        MSG_DRAW -> {
                            val time = getPlaybackTime()
                            draw(time)
                            if (!gotFirstFrame) {
                                gotFirstFrame = true
                                resetClock()
                            }
                            this@NopeTextureView.time = time
                        }

                        MSG_RELEASE -> {
                            stopped = true
                            paused = true
                            Choreographer.getInstance().removeFrameCallback(this@NopeTextureView)
                            releaseContext()
                            quitSafely()
                        }

                        MSG_SEEK -> {
                            time = msg.obj as Double
                            frameIndex = (time * frameRate.num / frameRate.den).toLong()
                            gotFirstFrame = false
                            resetClock()
                        }
                    }
                }
            }.apply {
                sendMessage(obtainMessage(MSG_INIT))
                sendMessage(obtainMessage(MSG_DRAW))
            }
        }
    }

    override fun onSurfaceTextureAvailable(surface: SurfaceTexture, width: Int, height: Int) {
    }

    override fun onSurfaceTextureSizeChanged(surface: SurfaceTexture, width: Int, height: Int) {
        handler.apply {
            sendMessage(obtainMessage(MSG_RESIZE, width, height))
            sendMessage(obtainMessage(MSG_DRAW))
        }
    }

    override fun onSurfaceTextureDestroyed(surface: SurfaceTexture): Boolean {
        handler.apply { sendMessage(obtainMessage(MSG_STOP)) }
        return true
    }

    override fun onSurfaceTextureUpdated(surface: SurfaceTexture) {
    }

    private fun setupContext() {
        nativeWindow = NGLContext.createNativeWindow(Surface(surfaceTexture))

        ctx = NGLContext().apply {
            val config = NGLConfig.Builder
                .setWindow(nativeWindow)
                .setBackend(NGLConfig.BACKEND_OPENGLES)
                .setClearColor(0f, 0f, 0f, 1f)
                .setSwapInterval(-1)
                .build()
            configure(config)
        }
    }

    private fun releaseContext() {
        ctx?.release()
        if (nativeWindow != 0L) {
            NGLContext.releaseNativeWindow(nativeWindow)
            nativeWindow = 0
        }
    }

    fun draw(time: Double) {
        ctx?.draw(time)
    }

    fun release() {
        handler.apply {
            sendMessageAtFrontOfQueue(obtainMessage(MSG_RELEASE))
        }
    }

    fun setScene(scene: NGLScene?) {
        handler.apply {
            sendMessageAtFrontOfQueue(obtainMessage(MSG_SET_SCENE, scene))
        }
    }

    fun start() {
        handler.apply {
            sendMessageAtFrontOfQueue(obtainMessage(MSG_START))
        }
    }

    fun stop() {
        handler.apply {
            sendMessageAtFrontOfQueue(obtainMessage(MSG_STOP))
        }
    }

    fun pause() {
        handler.apply {
            sendMessageAtFrontOfQueue(obtainMessage(MSG_PAUSE))
        }
    }

    companion object {
        private const val MSG_INIT = 0
        private const val MSG_SET_SCENE = 1
        private const val MSG_DRAW = 2
        private const val MSG_START = 3
        private const val MSG_PAUSE = 4
        private const val MSG_STOP = 5
        private const val MSG_RELEASE = 6
        private const val MSG_RESIZE = 7
        private const val MSG_STEP = 8
        private const val MSG_SEEK = 9
    }

    private fun resetClock() {
        val now = System.nanoTime()
        val frameTsNano: Long = frameIndex * 1000000000L * frameRate.den / frameRate.num
        clockOffset = now - frameTsNano
    }

    fun getTime(): Double {
        return time
    }

    private fun getPlaybackTime(): Double {
        if (paused) {
            return frameIndex * frameRate.den / frameRate.num.toDouble()
        }

        val frameTimeNanos = System.nanoTime()
        var playbackTimeNanos = frameTimeNanos - clockOffset
        if (clockOffset < 0 || playbackTimeNanos / 1e9 > duration) {
            clockOffset = frameTimeNanos
            gotFirstFrame = false
            playbackTimeNanos = 0
        }

        val newFrameIndex: Long =
            playbackTimeNanos * frameRate.num / (frameRate.den * 1000000000L)
        frameIndex = newFrameIndex

        return frameIndex * frameRate.den / frameRate.num.toDouble()
    }

    fun step(step: Int) {
        handler.apply {
            sendMessage(obtainMessage(MSG_STEP, step))
        }
    }

    fun seek(time: Double) {
        handler.apply {
            removeMessages(MSG_SEEK)
            sendMessage(obtainMessage(MSG_SEEK, time))
        }
    }

    override fun doFrame(frameTimeNanos: Long) {
        handler.apply {
            removeMessages(MSG_DRAW)
            sendMessage(obtainMessage(MSG_DRAW))
        }
        if (!paused && !stopped) Choreographer.getInstance().postFrameCallback(this)
    }
}
