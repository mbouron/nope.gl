/*
 * Copyright 2024 Matthieu Bouron <matthieu.bouron@gmail.com>
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

package org.nopeforge.nopegl.engine

import android.graphics.SurfaceTexture
import android.os.HandlerThread
import android.os.Looper
import android.os.Message
import android.util.Rational
import android.util.Size
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.TextureView
import android.view.TextureView.SurfaceTextureListener
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.android.asCoroutineDispatcher
import kotlinx.coroutines.withContext
import org.nopeforge.nopegl.NGLConfig
import org.nopeforge.nopegl.NGLContext
import org.nopeforge.nopegl.NGLRational
import org.nopeforge.nopegl.NGLScene
import timber.log.Timber
import java.util.concurrent.CopyOnWriteArraySet
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.math.ceil
import kotlin.time.Duration
import kotlin.time.Duration.Companion.seconds
import kotlin.time.DurationUnit
import kotlin.time.TimeSource

class EngineRenderer {
    private val clock: Clock = Clock()

    private val clockListener = Clock.Listener { time ->
        if (isDrawing()) {
            draw(time)
        }
    }
    private var scene: NGLScene? = null
    private val thread = HandlerThread("Engine").apply { start() }
    private val handler: Handler = Handler(thread.looper)
    private val dispatcher = handler.asCoroutineDispatcher()

    private var position: Duration = Duration.INFINITE
    val time: Duration
        get() = clock.time

    var looping: Boolean = false
        set(value) {
            field = value
            clock.looping = value
        }

    @Volatile
    var playWhenReady: Boolean = false
        set(value) {
            field = value
            if (isReady()) {
                if (value) {
                    play()
                } else {
                    pause()
                }
            }
        }

    private var state: State = State.Init
        set(value) {
            field = value
            when (value) {
                State.Init -> Unit /* no-op */
                State.Initialized -> playbackCallbacks.onEach { it.onInitialized() }
                State.Ready -> playbackCallbacks.onEach { it.onReady() }
                State.Started -> playbackCallbacks.onEach { it.onStarted() }
                State.Paused -> playbackCallbacks.onEach { it.onPaused() }
                State.Stopped -> playbackCallbacks.onEach { it.onStopped() }
                State.Released -> playbackCallbacks.onEach { it.onReleased() }
                State.Disposed -> Unit /* no-op */
            }
            val id = value.id
            playbackCallbacks.onEach { it.onStateChanged(id) }
        }

    private val playbackCallbacks = CopyOnWriteArraySet<PlaybackCallback>()

    private var windowPointer: Long = 0
    private var engine: NGLContext? = null

    private var size: Size? = null

    init {
        clock.listener = clockListener
    }

    fun initOffscreen(width: Int, height: Int) {
        handler.sendMessageAtFrontOfQueue(handler.obtainMessage(MSG_INIT, width, height))
    }

    fun init(surface: Surface) {
        handler.sendMessageAtFrontOfQueue(handler.obtainMessage(MSG_INIT, surface))
    }

    fun play() {
        handler.sendMessage(handler.obtainMessage(MSG_START))
    }

    fun pause() {
        handler.removeMessages(MSG_DRAW)
        handler.removeMessages(MSG_PAUSE)
        handler.removeMessages(MSG_START)
        handler.sendMessage(handler.obtainMessage(MSG_PAUSE))
    }

    fun stop() {
        handler.removeCallbacksAndMessages(null)
        handler.sendMessageAtFrontOfQueue(handler.obtainMessage(MSG_STOP))
    }

    fun release(blocking: Boolean = false) {
        val latch = CountDownLatch(1)
        handler.removeCallbacksAndMessages(null)
        handler.sendMessageAtFrontOfQueue(handler.obtainMessage(MSG_RELEASE, latch))
        if (blocking) {
            if (!latch.await(RELEASE_TIMEOUT_MS, TimeUnit.MILLISECONDS)) {
                Timber.e("Engine release timed out after ${RELEASE_TIMEOUT_MS}ms with state=$state")
            }
        }
    }

    fun dispose() {
        handler.removeCallbacksAndMessages(null)
        handler.sendMessageAtFrontOfQueue(handler.obtainMessage(MSG_DISPOSE))
        thread.quitSafely()
    }

    fun setScene(scene: NGLScene?) {
        handler.removeMessages(MSG_SET_SCENE)
        handler.sendMessage(handler.obtainMessage(MSG_SET_SCENE, scene))
    }

    fun step(step: Int) {
        handler.sendMessage(handler.obtainMessage(MSG_STEP, step))
    }

    fun seek(position: Duration) {
        handler.removeMessages(MSG_SEEK)
        handler.removeMessages(MSG_DRAW)
        handler.sendMessage(handler.obtainMessage(MSG_SEEK, position))
    }

    fun addPlaybackCallback(callback: PlaybackCallback) {
        playbackCallbacks.add(callback)
    }

    fun removePlaybackCallback(callback: PlaybackCallback) {
        playbackCallbacks.remove(callback)
    }

    private fun clearPlaybackCallbacks() {
        playbackCallbacks.clear()
    }

    private fun onInit(width: Int, height: Int) {
        reset()
        updateState(State.Init)
        onInit(
            NGLConfig.Builder()
                .setOffscreen(true)
                .setWidth(width)
                .setHeight(height),
        )
    }

    private fun onInit(surface: Surface) {
        reset()
        updateState(State.Init)
        windowPointer = (NGLContext.createNativeWindow(surface))
        val configBuilder = NGLConfig.Builder()
            .setWindow(windowPointer)
        onInit(configBuilder)
    }

    private fun onInit(configBuilder: NGLConfig.Builder) {
        val config = configBuilder
            .setBackend(NGLConfig.BACKEND_OPENGLES)
            .setClearColor(0f, 0f, 0f, 1f)
            .build()

        engine = NGLContext().apply {
            require(configure(config) == 0) {
                "Failed to configure"
            }
        }
        updateState(State.Initialized)
        loadSceneIfInitialized()
    }

    fun resize(width: Int, height: Int) {
        handler.removeMessages(MSG_DRAW)
        handler.sendMessage(handler.obtainMessage(MSG_RESIZE, width, height))
    }

    suspend fun <T> refresh(block: suspend CoroutineScope.() -> T) {
        withContext(dispatcher) {
            onDrawFrame(time)
            block()
        }
    }
    fun refresh() {
        handler.removeMessages(MSG_REFRESH_DURATION)
        handler.sendMessage(handler.obtainMessage(MSG_REFRESH_DURATION))
        draw()
    }

    private fun draw(time: Duration = this.time) {
        handler.removeMessages(MSG_DRAW)
        val message = handler.obtainMessage(MSG_DRAW, time)
        handler.sendMessage(message)
    }

    private fun onStart() {
        if (!isReady()) return
        updateState(State.Started)
        clock.play()
    }

    private fun onPause() {
        if (!isReady()) return
        updateState(State.Paused)
        clock.pause()
    }

    private fun onStop() {
        if (!isReady()) return
        updateState(State.Stopped)
        clock.stop()
    }

    private fun onSetScene(scene: NGLScene?) {
        engine?.resetScene()
        this.scene = scene
        loadSceneIfInitialized()
    }

    private fun loadVideoScene() {
        val timeMark = TimeSource.Monotonic.markNow()
        engine?.resetScene()
        val loadedScene = scene
        if (loadedScene == null) {
            clock.duration = Duration.ZERO
            clock.frameRate = DEFAULT_FRAME_RATE
        } else {
            clock.frameRate = loadedScene.frameRate.toRational()
            clock.duration = loadedScene.duration.seconds
            val result = engine?.setScene(loadedScene) ?: 0
            require(result == 0) { "Failed to set scene" }
        }
        onDrawFrame(time)
        val elapsed = timeMark.elapsedNow()
        playbackCallbacks.onEach { it.onSceneChanged(loadedScene, elapsed) }
        updateState(State.Ready)
        if (playWhenReady) {
            onStart()
        }
    }

    private fun loadSceneIfInitialized() {
        if (!isInitialized()) return

        try {
            loadVideoScene()
        } catch (cause: Throwable) {
            Timber.w(cause, "Failed to load scene")
            playbackCallbacks.onEach { it.onLoadSceneError(cause) }
            clock.stop()
            engine?.resetScene()
        }
    }

    private fun onStep(step: Int) {
        if (!isReady()) return
        onPause()
        clock.step(step)
    }

    private fun onDrawFrame(position: Duration) {
        val engine = engine ?: return
        engine.draw(position.toDouble(DurationUnit.SECONDS))
        if (position != this.position) {
            playbackCallbacks.onEach { it.onPositionChanged(position) }
        }
        this.position = position
    }

    private fun onSeek(position: Duration) {
        if (!isReady()) return
        val scene = scene ?: return
        val fps = scene.frameRate.toDouble()
        clock.seek(nextFramePosition(position, fps))
    }

    private fun nextFramePosition(time: Duration, fps: Double): Duration {
        return (ceil(time.toDouble(DurationUnit.SECONDS) * fps) / fps).seconds
    }

    private fun onResize(width: Int, height: Int) {
        if (!isReady()) return
        engine?.resize(width, height)
        size = Size(width, height)
        engine?.draw(time.toDouble(DurationUnit.SECONDS))
    }

    private fun onRelease(sync: CountDownLatch) {
        reset()
        updateState(State.Released)
        sync.countDown()
    }

    private fun reset() {
        clock.stop()
        position = Duration.INFINITE
        engine?.resetScene()
        engine?.release()
        engine = null
        if (windowPointer != 0L) {
            NGLContext.releaseNativeWindow(windowPointer)
        }
        windowPointer = 0
    }

    private fun onSeek(time: Double) {
        clock.seek(time.seconds)
    }

    private fun onRefreshDuration() {
        clock.duration = scene?.duration?.seconds ?: Duration.ZERO
    }

    private fun onDispose() {
        reset()
        updateState(State.Disposed)
    }

    private fun updateState(newState: State) {
        synchronized(state) {
            state = newState
        }
    }

    private fun isInitialized() = engine != null && state in State.Initialized..State.Paused
    private fun isReady() = engine != null && state in State.Ready..State.Paused
    private fun isDrawing() = engine != null && state in State.Started..State.Paused

    inner class Handler(looper: Looper) : android.os.Handler(looper) {

        override fun handleMessage(msg: Message) {
            if (state == State.Disposed) return

            when (msg.what) {
                MSG_INIT -> {
                    val surface = msg.obj as? Surface
                    if (surface == null) {
                        onInit(msg.arg1, msg.arg2)
                    } else {
                        onInit(surface)
                    }
                }
                MSG_START -> onStart()
                MSG_PAUSE -> onPause()
                MSG_STOP -> onStop()
                MSG_SET_SCENE -> onSetScene(msg.obj as NGLScene?)
                MSG_STEP -> onStep(msg.obj as Int)
                MSG_RESIZE -> onResize(msg.arg1, msg.arg2)
                MSG_DRAW -> onDrawFrame(msg.obj as Duration)
                MSG_RELEASE -> onRelease(msg.obj as CountDownLatch)
                MSG_SEEK -> onSeek(msg.obj as Duration)
                MSG_REFRESH_DURATION -> onRefreshDuration()
                MSG_DISPOSE -> onDispose()
            }
        }
    }

    companion object {
        private const val MSG_INIT = 0x00
        private const val MSG_SET_SCENE = 0x01
        private const val MSG_DRAW = 0x02
        private const val MSG_START = 0x03
        private const val MSG_PAUSE = 0x04
        private const val MSG_STOP = 0x05
        private const val MSG_RELEASE = 0x06
        private const val MSG_RESIZE = 0x07
        private const val MSG_STEP = 0x08
        private const val MSG_SEEK = 0x09
        private const val MSG_REFRESH_DURATION = 0x0A
        private const val MSG_DISPOSE = 0x0E
        private val DEFAULT_FRAME_RATE = Rational(60, 1)
        private const val RELEASE_TIMEOUT_MS = 1000L
    }

    private enum class State(val id: Int) {
        Init(0),
        Initialized(1),
        Ready(2),
        Started(3),
        Paused(4),
        Stopped(5),
        Released(6),
        Disposed(7),
    }

    interface PlaybackCallback {
        fun onStateChanged(state: Int) = Unit
        fun onInitialized() = Unit
        fun onReady() = Unit
        fun onStarted() = Unit
        fun onPaused() = Unit
        fun onSceneChanged(scene: NGLScene?, elapsed: Duration?) = Unit
        fun onLoadSceneError(cause: Throwable) = Unit
        fun onPositionChanged(position: Duration) = Unit
        fun onStopped() = Unit
        fun onReleased() = Unit
    }
}

fun EngineRenderer.attach(surfaceView: SurfaceView) = attach(surfaceView.holder)

fun EngineRenderer.attach(holder: SurfaceHolder) {
    holder.addCallback(
        object : SurfaceHolder.Callback2 {
            override fun surfaceCreated(holder: SurfaceHolder) {
                init(holder.surface)
            }

            override fun surfaceChanged(
                holder: SurfaceHolder,
                format: Int,
                width: Int,
                height: Int,
            ) {
                Timber.d("Surface resize: $width, $height")
                resize(width, height)
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                // release before surface is destroyed
                release(true)
            }

            override fun surfaceRedrawNeeded(holder: SurfaceHolder) {
                refresh()
            }
        },
    )
}

fun EngineRenderer.attach(textureView: TextureView) {
    textureView.surfaceTextureListener = object : SurfaceTextureListener {
        override fun onSurfaceTextureAvailable(
            surfaceTexture: SurfaceTexture,
            width: Int,
            height: Int,
        ) {
            Surface(surfaceTexture).also {
                init(it)
                resize(width, height)
            }
        }

        override fun onSurfaceTextureSizeChanged(
            surfaceTexture: SurfaceTexture,
            width: Int,
            height: Int,
        ) {
            Timber.d("Surface resize: $width, $height")
            resize(width, height)
        }

        override fun onSurfaceTextureDestroyed(surfaceTexture: SurfaceTexture): Boolean {
            release(true)
            textureView.surfaceTextureListener = null
            return true
        }

        override fun onSurfaceTextureUpdated(surfaceTexture: SurfaceTexture) {
            refresh()
        }

    }
}

fun NGLRational.toDouble(): Double = num.toDouble() / den.toDouble()

fun NGLRational.toFloat(): Float = num / den.toFloat()

fun Rational.toNGLRational() = NGLRational(num = numerator, den = denominator)

fun NGLRational.toRational() = Rational(num, den)
