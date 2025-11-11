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

package org.nopeforge.nopegl.viewmodels

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.nopeforge.nopegl.NGLScene
import org.nopeforge.nopegl.engine.EngineRenderer
import timber.log.Timber
import kotlin.time.Duration

class PlayerViewModel(initialBackend: Int) : ViewModel() {

    private var _renderer: EngineRenderer? = null
    val renderer: EngineRenderer
        get() = _renderer ?: throw IllegalStateException("Renderer not initialized")

    private val _sliderPosition = MutableStateFlow(Duration.ZERO)
    val sliderPosition: StateFlow<Duration> = _sliderPosition.asStateFlow()

    private val _isSeeking = MutableStateFlow(false)
    val isSeeking: StateFlow<Boolean> = _isSeeking.asStateFlow()

    private val _isPlaying = MutableStateFlow(false)
    val isPlaying: StateFlow<Boolean> = _isPlaying.asStateFlow()

    private val _backend = MutableStateFlow(initialBackend)
    val backend: StateFlow<Int> = _backend.asStateFlow()

    private val _desiredPlayingState = MutableStateFlow(true)
    private val _isLifecycleResumed = MutableStateFlow(true)

    private var playbackCallback: EngineRenderer.PlaybackCallback? = null
    private var currentScene: NGLScene? = null
    private var timePollingJob: Job? = null

    init {
        initializeRenderer(initialBackend)
        // Set default looping behavior
        renderer.looping = true
        startTimePolling()
    }

    private fun startTimePolling() {
        timePollingJob?.cancel()
        timePollingJob = viewModelScope.launch {
            while (isActive) {
                if (!_isSeeking.value) {
                    _renderer?.let { renderer ->
                        _sliderPosition.value = renderer.time
                    }
                }
                delay(16) // ~60fps updates
            }
        }
    }

    fun setLifecycleResumed(resumed: Boolean) {
        _isLifecycleResumed.value = resumed
        updatePlayWhenReady()
    }

    private fun updatePlayWhenReady() {
        renderer.playWhenReady = _desiredPlayingState.value && _isLifecycleResumed.value
    }

    private fun initializeRenderer(backend: Int) {
        val preservedLooping = _renderer?.looping ?: true
        val preservedScene = currentScene

        _renderer?.let { oldRenderer ->
            playbackCallback?.let { oldRenderer.removePlaybackCallback(it) }
            oldRenderer.release(blocking = true)
            oldRenderer.dispose()
        }

        val newRenderer = EngineRenderer(backend)
        _renderer = newRenderer

        playbackCallback = object : EngineRenderer.PlaybackCallback {
            override fun onStateChanged(state: Int) {
                Timber.d("State changed: $state")
            }

            override fun onStarted() {
                _isPlaying.value = true
            }

            override fun onStopped() {
                _isPlaying.value = false
            }

            override fun onPaused() {
                _isPlaying.value = false
            }

            override fun onReleased() {
                _isPlaying.value = false
            }

            override fun onSceneChanged(scene: NGLScene?, elapsed: Duration?) {
                if (scene != null) {
                    Timber.d("Scene loaded in $elapsed")
                }
            }
        }

        playbackCallback?.let { newRenderer.addPlaybackCallback(it) }

        newRenderer.looping = preservedLooping
        updatePlayWhenReady()
        if (preservedScene != null) {
            newRenderer.setScene(preservedScene)
        }
    }

    fun setScene(scene: NGLScene?) {
        currentScene = scene
        viewModelScope.launch {
            renderer.setScene(scene)
        }
    }

    fun play() {
        _desiredPlayingState.value = true
        updatePlayWhenReady()
        renderer.play()
    }

    fun pause() {
        _desiredPlayingState.value = false
        renderer.pause()
    }

    fun stop() {
        renderer.stop()
    }

    fun startSeeking() {
        _isSeeking.value = true
    }

    fun seek(position: Duration) {
        _isSeeking.value = true
        _sliderPosition.value = position
        _renderer?.seek(position)
    }

    fun finishSeeking() {
        _isSeeking.value = false
    }

    fun step(frames: Int) {
        renderer.step(frames)
    }

    fun setLooping(looping: Boolean) {
        _renderer?.let { it.looping = looping }
    }

    fun setPlayWhenReady(playWhenReady: Boolean) {
        _renderer?.let { it.playWhenReady = playWhenReady }
    }

    fun setBackend(newBackend: Int) {
        if (_backend.value != newBackend) {
            _backend.value = newBackend
            initializeRenderer(newBackend)
        }
    }

    override fun onCleared() {
        super.onCleared()
        timePollingJob?.cancel()
        timePollingJob = null
        _renderer?.let { renderer ->
            playbackCallback?.let { renderer.removePlaybackCallback(it) }
            renderer.release()
            renderer.dispose()
        }
        _renderer = null
        playbackCallback = null
    }
}
